/* SPDX-License-Identifier: Apache-2.0 */
/*
 * RADIO backend that shares the radio with Bluetooth LE. Each exchange runs
 * inside one MPSL timeslot, driven by RADIO and TIMER0 signals, so the
 * SoftDevice Controller keeps its own connection events. See docs/bluetooth.md.
 */
#include "radio.h"
#include "radio_regs.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_timer.h>
#include <mpsl_timeslot.h>

LOG_MODULE_REGISTER(radio, LOG_LEVEL_INF);

/* Ramp-up plus TX/RX turnaround, with fast ramp-up enabled. */
#define RAMP_US 40U
/* 2 Mbit/s: preamble, address, length, S1, payload and CRC at 4 us per byte. */
#define AIR_US_PER_BYTE 4U
#define AIR_OVERHEAD_BYTES 11U
/* Extra time to finish a packet whose address arrived just before the RX deadline. */
#define RX_TAIL_US (AIR_US_PER_BYTE * (RADIO_PACKET_SIZE + AIR_OVERHEAD_BYTES))
/* Setup at timeslot start and teardown before it ends. */
#define SLOT_MARGIN_US 150U
/* MPSL refuses an EARLIEST request whose timeout is too short to schedule. */
#define MIN_REQUEST_TIMEOUT_US 200U
#define WAIT_POLL_US 50U
#define DISABLE_SPIN 400U

enum phase {
	PHASE_IDLE,
	PHASE_PENDING,
	PHASE_TX,
	PHASE_RX,
	PHASE_DONE,
	/* The thread gave up; the callback returns the session to idle. */
	PHASE_ABANDONED,
};

static uint8_t rx_scratch[RADIO_PACKET_SIZE] __aligned(4);
static uint8_t tx_scratch[RADIO_PACKET_SIZE] __aligned(4);
static mpsl_timeslot_session_id_t session;
static mpsl_timeslot_signal_return_param_t signal_return;
static mpsl_timeslot_request_t request;
static atomic_t phase;
static bool initialized;

/* Written by the thread before PENDING; read by the callback afterwards. */
static struct radio_addr job_addr;
static uint8_t job_channel;
static uint32_t job_rx_timeout_us;
static size_t job_rx_capacity;

/* Written by the callback before DONE; read by the thread afterwards. */
static int job_result;
static uint8_t last_rssi;
static bool rx_extended;

static struct radio_timeslot_diag diag;

void radio_timeslot_diag_get(struct radio_timeslot_diag *out)
{
	*out = diag;
}

uint32_t radio_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static uint32_t slot_now_us(void)
{
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CAPTURE2);
	return nrf_timer_cc_get(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL2);
}

static void timer_arm(nrf_timer_cc_channel_t channel, uint32_t at_us)
{
	nrf_timer_event_clear(MPSL_TIMER0, nrf_timer_compare_event_get(channel));
	nrf_timer_cc_set(MPSL_TIMER0, channel, at_us);
	nrf_timer_int_enable(MPSL_TIMER0, nrf_timer_compare_int_get(channel));
}

static void radio_stop(void)
{
	NRF_RADIO->INTENCLR = 0xFFFFFFFF;
	NRF_RADIO->SHORTS = 0;
	if (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
		NRF_RADIO->EVENTS_DISABLED = 0;
		NRF_RADIO->TASKS_DISABLE = 1;
		for (uint32_t i = 0; i < DISABLE_SPIN && !NRF_RADIO->EVENTS_DISABLED; i++) {
		}
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
}

static mpsl_timeslot_signal_return_param_t *finish(int result)
{
	radio_stop();
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK |
					   NRF_TIMER_INT_COMPARE1_MASK);
	job_result = result;
	diag.last_finish_us = slot_now_us();
	if (result == RADIO_RX_NONE) {
		diag.rx_none++;
	} else if (result == RADIO_RX_OK) {
		diag.rx_ok++;
	} else if (result == RADIO_RX_BAD_CRC || result == RADIO_RX_INVALID) {
		diag.rx_bad++;
	} else if (result == -EIO) {
		diag.tx_failures++;
	}
	atomic_set(&phase, PHASE_DONE);
	signal_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
	return &signal_return;
}

static mpsl_timeslot_signal_return_param_t *keep_going(void)
{
	signal_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	return &signal_return;
}

/* Record what the Bluetooth controller left behind; see docs/bluetooth.md. */
static void snapshot_hw(struct radio_timeslot_hw *hw)
{
#if defined(NRF52840_XXAA)
	hw->ppi_chen = NRF_PPI->CHEN;
	for (int i = 0; i < 4; i++) {
		hw->timer_cc[i] = NRF_TIMER0->CC[i];
	}
	hw->timer_shorts = NRF_TIMER0->SHORTS;
	hw->timer_inten = NRF_TIMER0->INTENSET;
	hw->power = NRF_RADIO->POWER;
	hw->state = NRF_RADIO->STATE;
	hw->shorts = NRF_RADIO->SHORTS;
	hw->inten = NRF_RADIO->INTENSET;
	hw->mode = NRF_RADIO->MODE;
	hw->modecnf0 = NRF_RADIO->MODECNF0;
	hw->pcnf0 = NRF_RADIO->PCNF0;
	hw->pcnf1 = NRF_RADIO->PCNF1;
	hw->crccnf = NRF_RADIO->CRCCNF;
	hw->tifs = NRF_RADIO->TIFS;
	hw->dacnf = NRF_RADIO->DACNF;
	hw->bcc = NRF_RADIO->BCC;
	hw->rxaddresses = NRF_RADIO->RXADDRESSES;
	hw->txaddress = NRF_RADIO->TXADDRESS;
	hw->frequency = NRF_RADIO->FREQUENCY;
	hw->txpower = NRF_RADIO->TXPOWER;
	hw->datawhiteiv = NRF_RADIO->DATAWHITEIV;
	hw->reg_173c = *(volatile uint32_t *)0x4000173CUL;
	hw->reg_1740 = *(volatile uint32_t *)0x40001740UL;
	hw->reg_1774 = *(volatile uint32_t *)0x40001774UL;
	hw->ccm_enable = NRF_CCM->ENABLE;
	hw->ccm_mode = NRF_CCM->MODE;
	hw->aar_enable = NRF_AAR->ENABLE;
#else
	memset(hw, 0, sizeof(*hw));
#endif
}

static mpsl_timeslot_signal_return_param_t *on_start(void)
{
	diag.starts++;
	snapshot_hw(&diag.hw_at_start);
	if (!atomic_cas(&phase, PHASE_PENDING, PHASE_TX)) {
		diag.abandoned++;
		atomic_cas(&phase, PHASE_ABANDONED, PHASE_IDLE);
		signal_return.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		return &signal_return;
	}
	/* Guard: end cleanly before the slot's length runs out. */
	timer_arm(NRF_TIMER_CC_CHANNEL0, request.params.earliest.length_us - SLOT_MARGIN_US);

	/*
	 * MPSL hands over a disabled RADIO but leaves the Bluetooth controller's
	 * register state in place, so reset only what this exchange uses rather
	 * than power-cycling the peripheral (the same approach Nordic's ESB takes).
	 */
	NRF_RADIO->INTENCLR = 0xFFFFFFFF;
	NRF_RADIO->SHORTS = 0;
	radio_regs_configure(job_channel, &job_addr);
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)tx_scratch;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
	/*
	 * MPSL forwards RADIO interrupts to this callback, but it does not enable
	 * the RADIO line in the NVIC for an application timeslot: without this the
	 * exchange completes on the air and nothing ever observes it.
	 */
	NVIC_ClearPendingIRQ(RADIO_IRQn);
	irq_enable(RADIO_IRQn);
	NRF_RADIO->TASKS_TXEN = 1;
	return keep_going();
}

static mpsl_timeslot_signal_return_param_t *start_rx(void)
{
	rx_scratch[0] = 0;
	rx_scratch[1] = 0;
	rx_extended = false;
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)rx_scratch;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk |
			    RADIO_SHORTS_DISABLED_RSSISTOP_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	atomic_set(&phase, PHASE_RX);
	NRF_RADIO->TASKS_RXEN = 1;
	diag.last_tx_done_us = slot_now_us();
	diag.last_rx_deadline_us = diag.last_tx_done_us + job_rx_timeout_us;
	timer_arm(NRF_TIMER_CC_CHANNEL1, diag.last_rx_deadline_us);
	return keep_going();
}

static mpsl_timeslot_signal_return_param_t *on_radio(void)
{
	if (!NRF_RADIO->EVENTS_DISABLED) {
		return keep_going();
	}
	NRF_RADIO->EVENTS_DISABLED = 0;

	if (atomic_get(&phase) == PHASE_TX) {
		if (!NRF_RADIO->EVENTS_END) {
			return finish(-EIO);
		}
		return job_rx_timeout_us ? start_rx() : finish(0);
	}
	if (atomic_get(&phase) != PHASE_RX) {
		return keep_going();
	}
	if (!NRF_RADIO->EVENTS_END) {
		return finish(RADIO_RX_NONE);
	}
	if (!(NRF_RADIO->CRCSTATUS & 1)) {
		return finish(RADIO_RX_BAD_CRC);
	}
	if (rx_scratch[0] < 1 || rx_scratch[0] > RADIO_MAX_PAYLOAD ||
	    (size_t)rx_scratch[0] + 2 > job_rx_capacity) {
		return finish(RADIO_RX_INVALID);
	}
	last_rssi = (uint8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
	return finish(RADIO_RX_OK);
}

static mpsl_timeslot_signal_return_param_t *on_timer(void)
{
	if (nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0)) {
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		diag.guard_timeouts++;
		diag.last_guard_us = slot_now_us();
		diag.last_guard_phase = (uint32_t)atomic_get(&phase);
		diag.last_guard_radio_state = NRF_RADIO->STATE;
		diag.last_guard_events = (NRF_RADIO->EVENTS_READY ? 1U : 0U) |
					 (NRF_RADIO->EVENTS_ADDRESS ? 2U : 0U) |
					 (NRF_RADIO->EVENTS_END ? 4U : 0U) |
					 (NRF_RADIO->EVENTS_DISABLED ? 8U : 0U);
		diag.last_guard_inten = NRF_RADIO->INTENSET;
		diag.last_guard_power = NRF_RADIO->POWER;
		return finish(-ETIMEDOUT);
	}
	if (!nrf_timer_event_check(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1)) {
		return keep_going();
	}
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE1);
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE1_MASK);
	if (atomic_get(&phase) != PHASE_RX) {
		return keep_going();
	}
	/* A packet already in flight gets time to finish; the RADIO signal ends it. */
	if (NRF_RADIO->EVENTS_ADDRESS && !rx_extended) {
		rx_extended = true;
		timer_arm(NRF_TIMER_CC_CHANNEL1, slot_now_us() + RX_TAIL_US);
		return keep_going();
	}
	return finish(RADIO_RX_NONE);
}

static mpsl_timeslot_signal_return_param_t *timeslot_callback(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	ARG_UNUSED(id);

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		return on_start();
	case MPSL_TIMESLOT_SIGNAL_RADIO:
		diag.radio_signals++;
		return on_radio();
	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		diag.timer_signals++;
		return on_timer();
	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
		diag.overstayed++;
		return finish(-ETIMEDOUT);
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			diag.blocked++;
		} else {
			diag.cancelled++;
		}
		/* The slot never started: let the link defer this exchange. */
		if (atomic_cas(&phase, PHASE_PENDING, PHASE_DONE)) {
			job_result = -EAGAIN;
		} else {
			atomic_cas(&phase, PHASE_ABANDONED, PHASE_IDLE);
		}
		return keep_going();
	default:
		return keep_going();
	}
}

/* MPSL calls must not be preempted by other threads. */
static int open_session(void)
{
	int err;

	k_sched_lock();
	err = mpsl_timeslot_session_open(timeslot_callback, &session);
	k_sched_unlock();
	return err;
}

int radio_init(void)
{
	int err;

	if (initialized) {
		return 0;
	}
	err = open_session();
	if (err) {
		LOG_ERR("timeslot session open failed (%d)", err);
		return err;
	}
	atomic_set(&phase, PHASE_IDLE);
	initialized = true;
	return 0;
}

void radio_reset(void)
{
	/* MPSL owns the RADIO between timeslots; each slot reconfigures it. */
	last_rssi = 0;
}

int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us)
{
	if (!radio_exchange_args_valid(channel, addr, tx, tx_len, rx, rx_capacity,
				       rx_timeout_us, budget_us)) {
		return -EINVAL;
	}
	if (!initialized) {
		return -ENODEV;
	}

	uint32_t length_us = SLOT_MARGIN_US * 2 + RAMP_US * 2 +
			     AIR_US_PER_BYTE * (uint32_t)(tx_len + AIR_OVERHEAD_BYTES) +
			     (rx_timeout_us ? rx_timeout_us + RX_TAIL_US : 0);

	if (budget_us < length_us + MIN_REQUEST_TIMEOUT_US) {
		return -EAGAIN;
	}
	/* A previous abandoned request still belongs to MPSL. */
	if (!atomic_cas(&phase, PHASE_IDLE, PHASE_PENDING)) {
		return -EBUSY;
	}

	memcpy(tx_scratch, tx, tx_len);
	job_addr = *addr;
	job_channel = channel;
	job_rx_timeout_us = rx_timeout_us;
	job_rx_capacity = rx_capacity;
	job_result = -EIO;
	request = (mpsl_timeslot_request_t){
		.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
		.params.earliest = {
			.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
			.priority = MPSL_TIMESLOT_PRIORITY_NORMAL,
			.length_us = length_us,
			.timeout_us = budget_us - length_us,
		},
	};

	uint32_t start = radio_now_us();

	diag.requests++;
	diag.last_length_us = length_us;
	k_sched_lock();
	int err = mpsl_timeslot_request(session, &request);

	k_sched_unlock();
	if (err) {
		diag.request_refused++;
		atomic_set(&phase, PHASE_IDLE);
		return -EAGAIN;
	}

	/* The slot starts within its timeout and lasts at most its length. */
	while (atomic_get(&phase) != PHASE_DONE) {
		if (radio_now_us() - start > budget_us + SLOT_MARGIN_US &&
		    atomic_cas(&phase, PHASE_PENDING, PHASE_ABANDONED)) {
			return -EAGAIN;
		}
		k_usleep(WAIT_POLL_US);
	}

	int result = job_result;

	if (result == RADIO_RX_OK) {
		memcpy(rx, rx_scratch, (size_t)rx_scratch[0] + 2);
	}
	atomic_set(&phase, PHASE_IDLE);
	return result;
}

uint8_t radio_last_rssi(void)
{
	return last_rssi;
}
