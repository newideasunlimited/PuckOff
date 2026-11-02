/* SPDX-License-Identifier: Apache-2.0 */
#include <mpsl_test_platform.h>
#include <stdio.h>
#include <string.h>

/* Exercise the production callbacks and exchange code, not a second driver. */
#include "../../src/ble/radio_mpsl.c"

NRF_RADIO_Type fake_radio;
NRF_TIMER_Type fake_timer;
NRF_RTC_Type fake_rtc;
NRF_PPI_Type fake_ppi;
void (*fake_completion_irq)(const void *);

enum scenario {
	REPLY, BAD_CRC, OVERSIZE, RX_TIMEOUT, CUTOFF, STUCK_DISABLE,
	BLOCKED, CANCELLED, NO_CALLBACK, LATE_START, LATE_THREAD, DELAYED_GRANT,
};
static enum scenario scenario;
static bool high_context, pending_irq, sdk_locked, sdk_busy;
static uint64_t wall_ticks;
static uint32_t current_slot_us;
static unsigned requests;
static int request_error;
static mpsl_timeslot_callback_t signal_callback;

static void advance_us(uint32_t us)
{
	wall_ticks += ((uint64_t)us * 32768 + 999999) / 1000000;
	fake_rtc.COUNTER = (uint32_t)wall_ticks & RF_WINDOW_RTC_MASK;
}

static uint8_t signal_at(uint32_t signal, uint32_t now)
{
	assert(now >= current_slot_us);
	advance_us(now - current_slot_us);
	current_slot_us = now;
	fake_timer.CC[3] = now;
	high_context = signal <= MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED;
	mpsl_timeslot_signal_return_param_t *result = signal_callback(0, signal);
	high_context = false;
	if (pending_irq) {
		pending_irq = false;
		fake_completion_irq(NULL);
	}
	return result ? result->callback_action : MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
}

static void simulate_grant(void)
{
	current_slot_us = 0;
	if (scenario == BLOCKED || scenario == CANCELLED) {
		NRF_RADIO_Type before = fake_radio;
		NRF_PPI_Type ppi_before = fake_ppi;

		assert(signal_at(scenario == BLOCKED ? MPSL_TIMESLOT_SIGNAL_BLOCKED :
				 MPSL_TIMESLOT_SIGNAL_CANCELLED, 0) ==
		       MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
		assert(memcmp(&before, &fake_radio, sizeof(before)) == 0);
		assert(memcmp(&ppi_before, &fake_ppi, sizeof(ppi_before)) == 0);
		return;
	}
	if (scenario == LATE_START) {
		NRF_RADIO_Type before = fake_radio;

		advance_us(3900);
		assert(signal_at(MPSL_TIMESLOT_SIGNAL_START, 0) ==
		       MPSL_TIMESLOT_SIGNAL_ACTION_END);
		assert(memcmp(&before, &fake_radio, sizeof(before)) == 0);
		return;
	}
	if (scenario == DELAYED_GRANT) {
		advance_us(2000);
	}
	assert(signal_at(MPSL_TIMESLOT_SIGNAL_START, 0) == MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
	assert(fake_ppi.CH[19].EEP == (uint32_t)(uintptr_t)&fake_timer.EVENTS_COMPARE[0]);
	assert(fake_ppi.CH[19].TEP == (uint32_t)(uintptr_t)&fake_radio.TASKS_DISABLE);
	assert(fake_timer.CC[0] <= 1800 && fake_timer.CC[2] <= 1900);
	if (scenario == DELAYED_GRANT) {
		assert(fake_timer.CC[0] < 1800);
	}
	assert(fake_radio.MODE == RADIO_MODE_MODE_Ble_2Mbit);
	assert(fake_radio.CRCPOLY == 0x11021 && fake_radio.CRCINIT == 0xffff);
	assert(fake_radio.PACKETPTR == (uint32_t)(uintptr_t)job.tx);
	assert(fake_radio.TASKS_TXEN == 1);
	fake_radio.STATE = RADIO_STATE_STATE_Disabled;
	fake_radio.EVENTS_END = 1;
	fake_radio.EVENTS_DISABLED = 1;
	fake_radio.EVENTS_READY = 1;
	fake_radio.EVENTS_ADDRESS = 1;
	uint8_t action = signal_at(MPSL_TIMESLOT_SIGNAL_RADIO, rf_window_tx_time(job.tx[0]) + 15);

	if (!job.rx_timeout_us) {
		assert(action == MPSL_TIMESLOT_SIGNAL_ACTION_END);
		assert(!fake_radio.EVENTS_READY && !fake_radio.EVENTS_ADDRESS);
		return;
	}
	assert(action == MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
	assert(fake_radio.PACKETPTR == (uint32_t)(uintptr_t)job.rx);
	assert(fake_radio.TASKS_RXEN == 1 && fake_timer.CC[1] > current_slot_us);
	fake_radio.STATE = 3;
	fake_radio.EVENTS_RSSIEND = 1;

	if (scenario == RX_TIMEOUT) {
		fake_timer.EVENTS_COMPARE[1] = 1;
		assert(signal_at(MPSL_TIMESLOT_SIGNAL_TIMER0, fake_timer.CC[1]) ==
		       MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
		assert(fake_radio.TASKS_DISABLE == 1 && fake_radio.SHORTS == 0);
		fake_radio.STATE = RADIO_STATE_STATE_Disabled;
		fake_radio.EVENTS_END = 0;
		fake_radio.EVENTS_DISABLED = 1;
		assert(signal_at(MPSL_TIMESLOT_SIGNAL_RADIO, current_slot_us + 6) ==
		       MPSL_TIMESLOT_SIGNAL_ACTION_END);
	} else if (scenario == CUTOFF || scenario == STUCK_DISABLE) {
		fake_timer.EVENTS_COMPARE[0] = 1;
		if (scenario == CUTOFF) {
			/* Model the PPI cutoff independently of the IRQ handler. */
			fake_radio.STATE = RADIO_STATE_STATE_Disabled;
			fake_radio.EVENTS_DISABLED = 1;
		}
		action = signal_at(MPSL_TIMESLOT_SIGNAL_TIMER0, fake_timer.CC[0]);
		if (scenario == CUTOFF) {
			assert(action == MPSL_TIMESLOT_SIGNAL_ACTION_END);
		} else {
			assert(action == MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
			fake_timer.EVENTS_COMPARE[2] = 1;
			assert(signal_at(MPSL_TIMESLOT_SIGNAL_TIMER0, fake_timer.CC[2]) ==
			       MPSL_TIMESLOT_SIGNAL_ACTION_END);
			assert(fake_radio.POWER == 1);
			fake_radio.STATE = RADIO_STATE_STATE_Disabled;
		}
	} else {
		job.rx[0] = scenario == OVERSIZE ? 97 : 3;
		job.rx[1] = 2;
		job.rx[2] = 0xf3;
		job.rx[3] = 0x12;
		job.rx[4] = 0x34;
		fake_radio.CRCSTATUS = scenario == BAD_CRC ? 0 : 1;
		fake_radio.RSSISAMPLE = 47;
		fake_radio.STATE = RADIO_STATE_STATE_Disabled;
		fake_radio.EVENTS_END = 1;
		fake_radio.EVENTS_DISABLED = 1;
		assert(signal_at(MPSL_TIMESLOT_SIGNAL_RADIO, current_slot_us + 200) ==
		       MPSL_TIMESLOT_SIGNAL_ACTION_END);
	}
	assert(fake_ppi.CH[19].EEP == 0 && fake_ppi.CH[19].TEP == 0);
	assert(fake_ppi.CHENCLR == BIT(19));
	assert(fake_radio.PACKETPTR == 0 && fake_radio.SHORTS == 0);
	assert(!fake_radio.EVENTS_READY && !fake_radio.EVENTS_ADDRESS && !fake_radio.EVENTS_RSSIEND);
	if (scenario == LATE_THREAD) {
		advance_us(5000);
	}
}

int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout)
{
	assert(!high_context);
	(void)timeout;
	if (mutex->held) { return -EBUSY; }
	mutex->held = true;
	return 0;
}
void k_mutex_unlock(struct k_mutex *mutex)
{
	assert(!high_context && mutex->held);
	mutex->held = false;
}
int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	assert(!high_context);
	if (!sem->count) {
		if (scenario == NO_CALLBACK) {
			advance_us(timeout);
			return -EAGAIN;
		}
		simulate_grant();
	}
	assert(sem->count);
	sem->count--;
	return 0;
}
void k_sem_give(struct k_sem *sem) { assert(!high_context); sem->count = 1; }
void k_sem_reset(struct k_sem *sem) { assert(!high_context); sem->count = 0; }
int64_t k_uptime_ticks(void) { assert(!high_context); return (int64_t)wall_ticks; }
uint64_t k_ticks_to_us_floor64(uint64_t ticks) { return ticks * 15625 / 512; }
void irq_enable(unsigned irq) { assert(!high_context && irq == SWI1_EGU1_IRQn); }
void NVIC_SetPendingIRQ(unsigned irq) { assert(irq == SWI1_EGU1_IRQn); pending_irq = true; }
void NVIC_ClearPendingIRQ(unsigned irq) { assert(irq == SWI1_EGU1_IRQn); pending_irq = false; }
void fake_log(const char *format, ...) { assert(!high_context); (void)format; }
void shell_print(const struct shell *shell, const char *format, ...)
{
	assert(!high_context); (void)shell; (void)format;
}
int multithreading_lock_acquire(k_timeout_t timeout)
{
	assert(!high_context && !sdk_locked);
	(void)timeout;
	if (sdk_busy) { return -EBUSY; }
	sdk_locked = true;
	return 0;
}
void multithreading_lock_release(void)
{
	assert(!high_context && sdk_locked); sdk_locked = false;
}
int mpsl_timeslot_session_open(mpsl_timeslot_callback_t callback,
			      mpsl_timeslot_session_id_t *id)
{
	assert(!high_context && sdk_locked);
	signal_callback = callback;
	*id = 0;
	return 0;
}
int mpsl_timeslot_request(mpsl_timeslot_session_id_t id, const mpsl_timeslot_request_t *req)
{
	assert(!high_context && sdk_locked && id == 0);
	assert(req->request_type == MPSL_TIMESLOT_REQ_TYPE_EARLIEST);
	assert(req->params.earliest.hfclk == MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED);
	assert(req->params.earliest.priority == MPSL_TIMESLOT_PRIORITY_NORMAL);
	assert(req->params.earliest.length_us == 2000);
	assert(req->params.earliest.timeout_us > 0 && req->params.earliest.timeout_us < 1750);
	requests++;
	return request_error;
}

int main(void)
{
	const struct radio_addr addr = { { 1, 2, 3, 4 }, 5 };
	uint8_t tx[] = { 1, 1, 0xe3 };
	uint8_t rx[RADIO_PACKET_SIZE], unchanged[RADIO_PACKET_SIZE];
	const enum scenario cases[] = {
		REPLY, BAD_CRC, OVERSIZE, RX_TIMEOUT, CUTOFF, STUCK_DISABLE,
		BLOCKED, CANCELLED, LATE_START, LATE_THREAD, DELAYED_GRANT,
	};
	const int expected[] = {
		RADIO_RX_OK, RADIO_RX_BAD_CRC, RADIO_RX_INVALID, RADIO_RX_NONE,
		-ETIMEDOUT, -ETIMEDOUT, -EAGAIN, -EAGAIN, -ETIMEDOUT, -ETIMEDOUT, RADIO_RX_OK,
	};

	memset(unchanged, 0xa5, sizeof(unchanged));
	assert(radio_init() == 0 && signal_callback && fake_completion_irq);
	assert(fake_radio.MODE == 0 && fake_ppi.CHENSET == 0);
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		scenario = cases[i];
		advance_us(5000);
		memcpy(rx, unchanged, sizeof(rx));
		int result = radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000);

		if (result != expected[i]) {
			fprintf(stderr, "scenario %d: result %d, expected %d, elapsed %u\n",
				scenario, result, expected[i], call_elapsed_us());
		}
		assert(result == expected[i]);
		if (expected[i] == RADIO_RX_OK) {
			const uint8_t packet[] = { 3, 2, 0xf3, 0x12, 0x34 };

			assert(memcmp(rx, packet, sizeof(packet)) == 0);
			assert(memcmp(rx + sizeof(packet), unchanged + sizeof(packet),
				      sizeof(rx) - sizeof(packet)) == 0);
			assert(radio_last_rssi() == 47);
		} else {
			assert(memcmp(rx, unchanged, sizeof(rx)) == 0);
		}
	}
	advance_us(5000);
	scenario = REPLY;
	assert(radio_exchange(18, &addr, tx, sizeof(tx), NULL, 0, 0, 4000) == RADIO_RX_NONE);
	unsigned before = requests;
	assert(radio_exchange(18, &addr, tx, sizeof(tx), NULL, 0, 0, 4000) == -EAGAIN);
	assert(requests == before);
	advance_us(5000);
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 2000) == -EAGAIN);
	assert(requests == before);
	sdk_busy = true;
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) == -EAGAIN);
	sdk_busy = false;
	request_error = -NRF_EAGAIN;
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) == -EAGAIN);
	request_error = 0;

	advance_us(5000);
	scenario = NO_CALLBACK;
	memcpy(rx, unchanged, sizeof(rx));
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) == -ETIMEDOUT);
	assert(memcmp(rx, unchanged, sizeof(rx)) == 0);
	radio_reset();
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) == -EBUSY);
	NRF_RADIO_Type before_late_start = fake_radio;
	current_slot_us = 0;
	assert(signal_at(MPSL_TIMESLOT_SIGNAL_START, 0) == MPSL_TIMESLOT_SIGNAL_ACTION_END);
	assert(memcmp(&before_late_start, &fake_radio, sizeof(fake_radio)) == 0);
	advance_us(5000);
	scenario = REPLY;
	assert(radio_exchange(18, &addr, tx, sizeof(tx), rx, 3, 1200, 4000) == RADIO_RX_INVALID);
	assert(memcmp(rx, unchanged, sizeof(rx)) == 0);

	struct radio_mpsl_stats stats;
	radio_mpsl_get_stats(&stats);
	assert(stats.denied == 2 && stats.forced_resets == 1);
	assert(stats.cutoffs == 2 && stats.completion_timeouts >= 2);
	assert(stats.max_call_us > 4000);
	puts("MPSL driver: real callbacks, staging, cutoffs, denial, quarantine and IRQ context passed");
	return 0;
}
