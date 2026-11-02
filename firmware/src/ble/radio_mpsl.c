/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 * nRF52840-only MPSL backend. RADIO/TIMER0/PPI19 are touched only in a grant.
 */
#include "../radio.h"
#include "radio_mpsl.h"
#include "rf_window.h"

#include <stdatomic.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <mpsl.h>
#include <mpsl_hwres.h>
#include <mpsl_timeslot.h>
#include <multithreading_lock.h>
#include <sdc_soc.h>
#include <nrfx.h>

LOG_MODULE_REGISTER(radio_mpsl, LOG_LEVEL_INF);

BUILD_ASSERT(IS_ENABLED(CONFIG_PUCKOFF_TRANSPORT_BLE_RF));
BUILD_ASSERT(IS_ENABLED(CONFIG_SOC_NRF52840) && IS_ENABLED(CONFIG_BT_LL_SOFTDEVICE));
BUILD_ASSERT(CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1);
BUILD_ASSERT(IS_ENABLED(CONFIG_NRF_RTC_TIMER) && CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == 32768);
BUILD_ASSERT(CONFIG_NRF_RTC_COUNTER_BIT_WIDTH == 24 && CONFIG_SYS_CLOCK_TICKS_PER_SEC == 32768,
	     "Absolute deadlines and timeout rounding require the SDK's full-width RTC timebase");
BUILD_ASSERT(ATOMIC_INT_LOCK_FREE == 2 && sizeof(unsigned int) == 4,
	     "Priority-zero callbacks need lock-free 32-bit atomics");
BUILD_ASSERT((MPSL_PPI_CHANNELS_USED_MASK & BIT(19)) != 0);
BUILD_ASSERT((SDC_PPI_CHANNELS_USED_MASK & BIT(19)) != 0);
BUILD_ASSERT(CONFIG_MPSL_LOW_PRIO_IRQN != SWI1_EGU1_IRQn);
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRFX_EGU1), "SWI1/EGU1 is reserved for RF completion");
BUILD_ASSERT(!DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(egu1)));
#if defined(CONFIG_NRF52_ANOMALY_109_WORKAROUND)
BUILD_ASSERT(CONFIG_NRF52_ANOMALY_109_WORKAROUND_EGU_INSTANCE != 1);
#endif

#define CUTOFF_PPI 19U
#define TIMER_INTERRUPTS (TIMER_INTENSET_COMPARE0_Msk | TIMER_INTENSET_COMPARE1_Msk | \
			  TIMER_INTENSET_COMPARE2_Msk)

enum job_state {
	JOB_FREE,
	JOB_PENDING,
	JOB_FINISHED,
};

static struct {
	struct radio_addr addr;
	struct rf_window window;
	uint32_t start_ticks;
	uint32_t budget_us;
	uint32_t rx_timeout_us;
	size_t rx_capacity;
	uint8_t channel;
	uint8_t rssi;
	uint8_t tx[RADIO_PACKET_SIZE] __aligned(4);
	uint8_t rx[RADIO_PACKET_SIZE] __aligned(4);
	int result;
} job;

static struct {
	atomic_uint requested;
	atomic_uint granted;
	atomic_uint denied;
	atomic_uint deferred;
	atomic_uint cutoffs;
	atomic_uint forced_resets;
	atomic_uint completion_timeouts;
	atomic_uint invalid_signals;
	atomic_uint max_call_us;
} counts;

static atomic_uint pending_job;
static atomic_bool abandoned;
static bool owns_window;
static bool initialized;
static bool requested_once;
static uint32_t last_request_us;
static uint8_t last_rssi;
static mpsl_timeslot_session_id_t session_id;
static mpsl_timeslot_request_t request;
static mpsl_timeslot_signal_return_param_t return_action;
static K_MUTEX_DEFINE(exchange_lock);
static K_SEM_DEFINE(completion, 0, 1);

static void count(atomic_uint *value)
{
	atomic_fetch_add_explicit(value, 1, memory_order_relaxed);
}

uint32_t radio_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static uint32_t call_elapsed_us(void)
{
	/* Read-only access to the SDK system clock; no Zephyr call at priority zero. */
	return rf_window_elapsed_us(job.start_ticks, NRF_RTC1->COUNTER);
}

static uint32_t slot_now_us(void)
{
	MPSL_TIMER0->TASKS_CAPTURE[3] = 1;
	return MPSL_TIMER0->CC[3];
}

static void completion_irq(const void *arg)
{
	ARG_UNUSED(arg);
	if (atomic_load_explicit(&pending_job, memory_order_acquire) == JOB_FINISHED) {
		k_sem_give(&completion);
	}
}

static void publish_result(int result)
{
	job.result = result;
	atomic_store_explicit(&pending_job, JOB_FINISHED, memory_order_release);
	NVIC_SetPendingIRQ(SWI1_EGU1_IRQn);
}

static mpsl_timeslot_signal_return_param_t *end_without_radio(int result)
{
	publish_result(result);
	return_action.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
	return &return_action;
}

static mpsl_timeslot_signal_return_param_t *finish_window(void)
{
	NRF_PPI->CHENCLR = BIT(CUTOFF_PPI);
	NRF_PPI->CH[CUTOFF_PPI].EEP = 0;
	NRF_PPI->CH[CUTOFF_PPI].TEP = 0;
	NRF_PPI->FORK[CUTOFF_PPI].TEP = 0;
	MPSL_TIMER0->INTENCLR = TIMER_INTERRUPTS;
	MPSL_TIMER0->EVENTS_COMPARE[0] = 0;
	MPSL_TIMER0->EVENTS_COMPARE[1] = 0;
	MPSL_TIMER0->EVENTS_COMPARE[2] = 0;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->INTENCLR = UINT32_MAX;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_RSSIEND = 0;
	NRF_RADIO->PACKETPTR = 0;
	owns_window = false;
	return end_without_radio(job.window.result);
}

static uint8_t reverse_byte(uint8_t value)
{
	value = (uint8_t)((value >> 4) | (value << 4));
	value = (uint8_t)(((value & 0xcc) >> 2) | ((value & 0x33) << 2));
	return (uint8_t)(((value & 0xaa) >> 1) | ((value & 0x55) << 1));
}

static void configure_radio(void)
{
	NRF_RADIO->INTENCLR = UINT32_MAX;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_2Mbit << RADIO_MODE_MODE_Pos;
	NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;
	NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->FREQUENCY = job.channel;
	NRF_RADIO->PCNF0 = (8U << RADIO_PCNF0_LFLEN_Pos) | (3U << RADIO_PCNF0_S1LEN_Pos);
	NRF_RADIO->PCNF1 = (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
			  (4U << RADIO_PCNF1_BALEN_Pos) |
			  (RADIO_MAX_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos);
	NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->CRCPOLY = 0x11021;
	NRF_RADIO->CRCINIT = 0xffff;
	NRF_RADIO->BASE0 = (uint32_t)reverse_byte(job.addr.base[0]) << 24 |
			   (uint32_t)reverse_byte(job.addr.base[1]) << 16 |
			   (uint32_t)reverse_byte(job.addr.base[2]) << 8 |
			   reverse_byte(job.addr.base[3]);
	NRF_RADIO->PREFIX0 = reverse_byte(job.addr.prefix);
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->RXADDRESSES = 1;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->EVENTS_READY = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->INTENSET = RADIO_INTENSET_DISABLED_Msk;
}

static void arm_cutoff(void)
{
	/* MPSL grants PPI19 specifically. Do not allocate or touch PPI17/18/20..31. */
	NRF_PPI->CHENCLR = BIT(CUTOFF_PPI);
	MPSL_TIMER0->INTENCLR = TIMER_INTERRUPTS;
	MPSL_TIMER0->EVENTS_COMPARE[0] = 0;
	MPSL_TIMER0->EVENTS_COMPARE[1] = 0;
	MPSL_TIMER0->EVENTS_COMPARE[2] = 0;
	MPSL_TIMER0->CC[0] = job.window.cutoff_us;
	MPSL_TIMER0->CC[2] = job.window.cutoff_us + RF_WINDOW_CLEANUP_US;
	NRF_PPI->CH[CUTOFF_PPI].EEP = (uint32_t)(uintptr_t)&MPSL_TIMER0->EVENTS_COMPARE[0];
	NRF_PPI->CH[CUTOFF_PPI].TEP = (uint32_t)(uintptr_t)&NRF_RADIO->TASKS_DISABLE;
	NRF_PPI->FORK[CUTOFF_PPI].TEP = 0;
	NRF_PPI->CHENSET = BIT(CUTOFF_PPI);
	MPSL_TIMER0->INTENSET = TIMER_INTENSET_COMPARE0_Msk | TIMER_INTENSET_COMPARE2_Msk;
}

static mpsl_timeslot_signal_return_param_t *apply_action(enum rf_window_action action)
{
	switch (action) {
	case RF_WINDOW_TRANSMIT:
		NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)job.tx;
		NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
		NRF_RADIO->TASKS_TXEN = 1;
		break;
	case RF_WINDOW_RECEIVE:
		NRF_RADIO->EVENTS_END = 0;
		NRF_RADIO->EVENTS_DISABLED = 0;
		NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)job.rx;
		NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |
				    RADIO_SHORTS_ADDRESS_RSSISTART_Msk |
				    RADIO_SHORTS_DISABLED_RSSISTOP_Msk;
		MPSL_TIMER0->EVENTS_COMPARE[1] = 0;
		MPSL_TIMER0->CC[1] = job.window.rx_deadline_us;
		MPSL_TIMER0->INTENSET = TIMER_INTENSET_COMPARE1_Msk;
		NRF_RADIO->TASKS_RXEN = 1;
		break;
	case RF_WINDOW_DISABLE:
		NRF_RADIO->SHORTS = 0;
		if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) {
			(void)rf_window_disabled(&job.window, slot_now_us(), false, false, 0);
			return finish_window();
		}
		NRF_RADIO->EVENTS_DISABLED = 0;
		NRF_RADIO->TASKS_DISABLE = 1;
		break;
	case RF_WINDOW_RESET_FINISH:
		/* Only this granted-window end guard may reset a wedged peripheral. */
		NRF_RADIO->POWER = 0;
		NRF_RADIO->POWER = 1;
		count(&counts.forced_resets);
		return finish_window();
	case RF_WINDOW_FINISH:
		return finish_window();
	case RF_WINDOW_NONE:
		break;
	}
	return_action.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	return &return_action;
}

static mpsl_timeslot_signal_return_param_t *start_window(void)
{
	uint32_t now = slot_now_us();
	uint32_t cutoff = rf_window_cutoff(now, job.budget_us, call_elapsed_us());

	count(&counts.granted);
	if (atomic_load_explicit(&pending_job, memory_order_acquire) != JOB_PENDING ||
	    atomic_load_explicit(&abandoned, memory_order_acquire) || cutoff == 0) {
		return end_without_radio(-EAGAIN);
	}
	enum rf_window_action action = rf_window_begin(&job.window, cutoff, now, job.tx[0],
						      job.rx_timeout_us, job.rx_capacity);

	if (action != RF_WINDOW_TRANSMIT) {
		return end_without_radio(job.window.result);
	}
	owns_window = true;
	arm_cutoff();
	if (NRF_RADIO->STATE != RADIO_STATE_STATE_Disabled) {
		return apply_action(rf_window_stop(&job.window, -EIO));
	}
	configure_radio();
	if (!rf_window_tx_fits(&job.window, slot_now_us(), job.tx[0])) {
		job.window.result = -EAGAIN;
		job.window.phase = RF_WINDOW_DONE;
		return finish_window();
	}
	return apply_action(RF_WINDOW_TRANSMIT);
}

static mpsl_timeslot_signal_return_param_t *timeslot_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	(void)id;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		return start_window();
	case MPSL_TIMESLOT_SIGNAL_RADIO:
	case MPSL_TIMESLOT_SIGNAL_TIMER0: {
		if (!owns_window) {
			return NULL;
		}
		uint32_t now = slot_now_us();

		if (MPSL_TIMER0->EVENTS_COMPARE[2]) {
			MPSL_TIMER0->EVENTS_COMPARE[2] = 0;
			return apply_action(rf_window_end_guard(&job.window,
					     NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled));
		}
		if (MPSL_TIMER0->EVENTS_COMPARE[0] || now >= job.window.cutoff_us) {
			MPSL_TIMER0->EVENTS_COMPARE[0] = 0;
			MPSL_TIMER0->INTENCLR = TIMER_INTENSET_COMPARE0_Msk;
			if (job.window.result != -ETIMEDOUT) {
				count(&counts.cutoffs);
			}
			return apply_action(rf_window_stop(&job.window, -ETIMEDOUT));
		}
		if (MPSL_TIMER0->EVENTS_COMPARE[1]) {
			MPSL_TIMER0->EVENTS_COMPARE[1] = 0;
			MPSL_TIMER0->INTENCLR = TIMER_INTENSET_COMPARE1_Msk;
			return apply_action(rf_window_stop(&job.window, RADIO_RX_NONE));
		}
		if (signal == MPSL_TIMESLOT_SIGNAL_RADIO && NRF_RADIO->EVENTS_DISABLED) {
			bool ended = NRF_RADIO->EVENTS_END != 0;
			bool crc_ok = (NRF_RADIO->CRCSTATUS & 1U) != 0;

			NRF_RADIO->EVENTS_DISABLED = 0;
			job.rssi = (uint8_t)(NRF_RADIO->RSSISAMPLE & 0x7f);
			return apply_action(rf_window_disabled(&job.window, now, ended,
							       crc_ok, job.rx[0]));
		}
		return NULL;
	}
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		count(&counts.denied);
		if (atomic_load_explicit(&pending_job, memory_order_acquire) == JOB_PENDING) {
			publish_result(-EAGAIN);
		}
		return NULL;
	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
		return NULL;
	default:
		/* INVALID_RETURN / OVERSTAYED are fatal SDK faults, not recoverable RF
		 * errors. In particular, do not touch RADIO after a revoked window.
		 */
		count(&counts.invalid_signals);
		return NULL;
	}
}

int radio_init(void)
{
	int err;

	if (initialized) {
		return 0;
	}
	err = multithreading_lock_acquire(K_MSEC(100));
	if (err) {
		return err;
	}
	err = mpsl_timeslot_session_open(timeslot_signal, &session_id);
	multithreading_lock_release();
	if (err) {
		LOG_ERR("MPSL session open failed (%d)", err);
		return err == -NRF_ENOMEM ? -ENOMEM : -EIO;
	}
	IRQ_CONNECT(SWI1_EGU1_IRQn, 1, completion_irq, NULL, 0);
	NVIC_ClearPendingIRQ(SWI1_EGU1_IRQn);
	irq_enable(SWI1_EGU1_IRQn);
	initialized = true;
	LOG_WRN("Experimental shared RF: 2ms grants, no guaranteed 250Hz rate");
	return 0;
}

void radio_reset(void)
{
	/* Quarantine a pending request. Its callback will end without RF or finish
	 * through the armed cutoff; a stack-owned RADIO must never be power-cycled.
	 */
	if (k_mutex_lock(&exchange_lock, K_NO_WAIT)) {
		LOG_ERR("Cannot reset an active MPSL exchange");
		return;
	}
	if (atomic_load_explicit(&pending_job, memory_order_acquire) == JOB_PENDING) {
		atomic_store_explicit(&abandoned, true, memory_order_release);
		LOG_WRN("MPSL reset deferred until the pending request retires");
	} else {
		atomic_store_explicit(&pending_job, JOB_FREE, memory_order_release);
	}
	k_mutex_unlock(&exchange_lock);
}

static int finish_call(int result)
{
	uint32_t elapsed = call_elapsed_us();
	uint32_t maximum = atomic_load_explicit(&counts.max_call_us, memory_order_relaxed);

	if (elapsed > maximum) {
		atomic_store_explicit(&counts.max_call_us, elapsed, memory_order_relaxed);
	}
	k_mutex_unlock(&exchange_lock);
	return result;
}

int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us)
{
	uint32_t start_ticks = NRF_RTC1->COUNTER;
	int err = rf_window_validate(channel, addr, tx, tx_len, rx, rx_capacity,
				     rx_timeout_us, budget_us);

	if (err) {
		LOG_ERR("Invalid MPSL RF transaction (%d)", err);
		return err;
	}
	if (!initialized) {
		return -ENODEV;
	}
	if (k_mutex_lock(&exchange_lock, K_NO_WAIT)) {
		return -EBUSY;
	}
	if (atomic_load_explicit(&pending_job, memory_order_acquire) == JOB_PENDING) {
		k_mutex_unlock(&exchange_lock);
		return -EBUSY;
	}
	uint32_t now_us = radio_now_us();

	if ((requested_once && now_us - last_request_us < RF_WINDOW_PERIOD_US) ||
	    !rf_window_request_timeout(budget_us,
					rf_window_elapsed_us(start_ticks, NRF_RTC1->COUNTER))) {
		count(&counts.deferred);
		k_mutex_unlock(&exchange_lock);
		return -EAGAIN;
	}
	job.start_ticks = start_ticks;
	job.budget_us = budget_us;
	job.addr = *addr;
	job.channel = channel;
	job.rx_timeout_us = rx_timeout_us;
	job.rx_capacity = rx_capacity;
	job.window.phase = RF_WINDOW_WAITING;
	memcpy(job.tx, tx, tx_len);
	memset(job.rx, 0, sizeof(job.rx));
	k_sem_reset(&completion);
	atomic_store_explicit(&abandoned, false, memory_order_release);

	err = multithreading_lock_acquire(K_NO_WAIT);
	if (err) {
		count(&counts.deferred);
		return finish_call(-EAGAIN);
	}
	uint32_t timeout = rf_window_request_timeout(budget_us, call_elapsed_us());

	if (!timeout) {
		multithreading_lock_release();
		count(&counts.deferred);
		return finish_call(-EAGAIN);
	}
	request = (mpsl_timeslot_request_t){
		.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
		.params.earliest = {
			.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
			.priority = MPSL_TIMESLOT_PRIORITY_NORMAL,
			.length_us = RF_WINDOW_LENGTH_US,
			.timeout_us = timeout,
		},
	};
	last_request_us = now_us;
	requested_once = true;
	atomic_store_explicit(&pending_job, JOB_PENDING, memory_order_release);
	count(&counts.requested);
	err = mpsl_timeslot_request(session_id, &request);
	multithreading_lock_release();
	if (err) {
		atomic_store_explicit(&pending_job, JOB_FREE, memory_order_release);
		if (err == -NRF_EAGAIN) {
			count(&counts.deferred);
			return finish_call(-EAGAIN);
		}
		LOG_ERR("MPSL request failed (%d)", err);
		return finish_call(err == -NRF_EINVAL ? -EINVAL : -EIO);
	}
	while (atomic_load_explicit(&pending_job, memory_order_acquire) != JOB_FINISHED) {
		uint32_t elapsed = call_elapsed_us();

		/* Leave two RTC ticks for timeout rounding and thread-side teardown. */
		if (elapsed + 62U >= budget_us ||
		    k_sem_take(&completion, K_USEC(budget_us - elapsed - 62U))) {
			atomic_store_explicit(&abandoned, true, memory_order_release);
			count(&counts.completion_timeouts);
			LOG_ERR_RATELIMIT("MPSL completion late; staging quarantined");
			return finish_call(-ETIMEDOUT);
		}
	}
	err = job.result;
	if (call_elapsed_us() + 62U >= budget_us) {
		count(&counts.completion_timeouts);
		LOG_ERR_RATELIMIT("MPSL result arrived after the caller's deadline");
		err = -ETIMEDOUT;
	}
	if (err == RADIO_RX_OK) {
		memcpy(rx, job.rx, (size_t)job.rx[0] + 2);
		last_rssi = job.rssi;
	}
	atomic_store_explicit(&pending_job, JOB_FREE, memory_order_release);
	return finish_call(err);
}

uint8_t radio_last_rssi(void)
{
	return last_rssi;
}

void radio_mpsl_get_stats(struct radio_mpsl_stats *out)
{
	out->requested = atomic_load_explicit(&counts.requested, memory_order_relaxed);
	out->granted = atomic_load_explicit(&counts.granted, memory_order_relaxed);
	out->denied = atomic_load_explicit(&counts.denied, memory_order_relaxed);
	out->deferred = atomic_load_explicit(&counts.deferred, memory_order_relaxed);
	out->cutoffs = atomic_load_explicit(&counts.cutoffs, memory_order_relaxed);
	out->forced_resets = atomic_load_explicit(&counts.forced_resets, memory_order_relaxed);
	out->completion_timeouts =
		atomic_load_explicit(&counts.completion_timeouts, memory_order_relaxed);
	out->invalid_signals = atomic_load_explicit(&counts.invalid_signals, memory_order_relaxed);
	out->max_call_us = atomic_load_explicit(&counts.max_call_us, memory_order_relaxed);
}

static int command_slots(const struct shell *shell, size_t argc, char **argv)
{
	struct radio_mpsl_stats stats;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	radio_mpsl_get_stats(&stats);
	shell_print(shell, "MPSL requested=%u granted=%u denied=%u deferred=%u",
		    stats.requested, stats.granted, stats.denied, stats.deferred);
	shell_print(shell, "cutoffs=%u forced_resets=%u late_completion=%u invalid_signals=%u",
		    stats.cutoffs, stats.forced_resets, stats.completion_timeouts,
		    stats.invalid_signals);
	shell_print(shell, "max_call_us=%u; nominal grant=2000us, cutoff<=1800us",
		    stats.max_call_us);
	return 0;
}
SHELL_CMD_REGISTER(rf_slots, NULL, "Read experimental MPSL scheduling counters", command_slots);
