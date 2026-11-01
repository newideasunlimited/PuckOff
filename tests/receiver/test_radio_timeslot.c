/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include <string.h>
#include "../../firmware/src/radio_timeslot.c"

NRF_RADIO_Type test_radio;
struct fake_timer test_timer;

/* What the simulated MPSL does with the next request. */
enum plan {
	PLAN_REPLY,
	PLAN_SILENT,
	PLAN_BAD_CRC,
	PLAN_LATE_REPLY,
	PLAN_BLOCKED,
	PLAN_NEVER,
};

static mpsl_timeslot_callback_t callback;
static mpsl_timeslot_request_t last_request;
static enum plan plan;
static int requests, request_error, open_error;
static bool scheduled;
static uint8_t last_action;
static const uint8_t reply[] = { 4, 2, 0xF1, 0xAA, 0xBB, 0xCC };

int32_t mpsl_timeslot_session_open(mpsl_timeslot_callback_t cb,
				   mpsl_timeslot_session_id_t *session_id)
{
	callback = cb;
	*session_id = 3;
	return open_error;
}

int32_t mpsl_timeslot_request(mpsl_timeslot_session_id_t session_id,
			      const mpsl_timeslot_request_t *req)
{
	CHECK(session_id == 3);
	requests++;
	if (request_error) {
		return request_error;
	}
	last_request = *req;
	scheduled = true;
	return 0;
}

static void send_signal(uint32_t sig)
{
	last_action = callback(3, sig)->callback_action;
}

/* Fire the TIMER0 compare channel if its interrupt is enabled. */
static void timer_fire(int channel)
{
	CHECK(test_timer.inten & (NRF_TIMER_INT_COMPARE0_MASK << channel));
	test_timer.now_us = test_timer.cc[channel];
	test_timer.event[channel] = true;
	send_signal(MPSL_TIMESLOT_SIGNAL_TIMER0);
}

static void radio_disabled(void)
{
	CHECK(test_radio.INTENSET & RADIO_INTENSET_DISABLED_Msk);
	test_radio.EVENTS_DISABLED = 1;
	send_signal(MPSL_TIMESLOT_SIGNAL_RADIO);
}

/* Runs the scheduled slot to completion the first time the thread polls. */
static void run_slot(void)
{
	if (!scheduled) {
		return;
	}
	scheduled = false;
	if (plan == PLAN_NEVER) {
		return;
	}
	if (plan == PLAN_BLOCKED) {
		send_signal(MPSL_TIMESLOT_SIGNAL_BLOCKED);
		return;
	}
	memset(&test_radio, 0, sizeof(test_radio));
	memset(&test_timer, 0, sizeof(test_timer));
	send_signal(MPSL_TIMESLOT_SIGNAL_START);
	CHECK(last_action == MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
	CHECK(test_radio.TASKS_TXEN == 1);
	CHECK(test_timer.inten & NRF_TIMER_INT_COMPARE0_MASK);
	CHECK(test_timer.cc[0] < last_request.params.earliest.length_us);

	test_timer.now_us = 150;
	test_radio.EVENTS_END = 1;
	radio_disabled();
	if (last_action == MPSL_TIMESLOT_SIGNAL_ACTION_END) {
		return;
	}
	CHECK(test_radio.TASKS_RXEN == 1);
	if (plan == PLAN_SILENT) {
		timer_fire(1);
		return;
	}
	if (plan == PLAN_LATE_REPLY) {
		test_radio.EVENTS_ADDRESS = 1;
		timer_fire(1);
		CHECK(last_action == MPSL_TIMESLOT_SIGNAL_ACTION_NONE);
	}
	/* PACKETPTR is 32 bits; on the host, compare instead of dereferencing it. */
	CHECK(test_radio.PACKETPTR == (uint32_t)(uintptr_t)rx_scratch);
	memcpy(rx_scratch, reply, sizeof(reply));
	test_radio.EVENTS_END = 1;
	test_radio.CRCSTATUS = plan == PLAN_BAD_CRC ? 0 : 1;
	test_radio.RSSISAMPLE = 0x31;
	radio_disabled();
}

static const struct radio_addr addr = { .base = { 1, 2, 3, 4 }, .prefix = 5 };
static const uint8_t poll[] = { 1, 1, 0xE3 };

static int exchange(enum plan p, uint8_t *rx, size_t capacity, uint32_t timeout_us)
{
	plan = p;
	return radio_exchange(18, &addr, poll, sizeof(poll), rx, capacity, timeout_us, 4000);
}

int main(void)
{
	uint8_t rx[RADIO_PACKET_SIZE];

	test_clock_hook = run_slot;
	CHECK(radio_exchange(18, &addr, poll, sizeof(poll), NULL, 0, 0, 4000) == -ENODEV);
	open_error = -ENOMEM;
	CHECK(radio_init() == -ENOMEM);
	open_error = 0;
	CHECK(radio_init() == 0);
	CHECK(radio_init() == 0);

	/* Argument and budget checks happen before MPSL is involved. */
	CHECK(radio_exchange(18, NULL, poll, sizeof(poll), NULL, 0, 0, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &addr, poll, sizeof(poll), rx, sizeof(rx), 1200, 1000) == -EAGAIN);
	CHECK(requests == 0);

	/* A poll and its reply inside one slot. */
	memset(rx, 0, sizeof(rx));
	CHECK(exchange(PLAN_REPLY, rx, sizeof(rx), 1200) == RADIO_RX_OK);
	CHECK(memcmp(rx, reply, sizeof(reply)) == 0);
	CHECK(radio_last_rssi() == 0x31);
	CHECK(last_action == MPSL_TIMESLOT_SIGNAL_ACTION_END);
	CHECK(test_radio.INTENCLR == 0xFFFFFFFF && test_radio.SHORTS == 0);
	CHECK(test_timer.inten == 0);
	CHECK(last_request.params.earliest.hfclk == MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED);
	CHECK(last_request.params.earliest.length_us + last_request.params.earliest.timeout_us ==
	      4000);
	CHECK(test_radio.BASE0 == ((uint32_t)bitrev8(1) << 24 | (uint32_t)bitrev8(2) << 16 |
				   (uint32_t)bitrev8(3) << 8 | bitrev8(4)));

	/* TX only ends as soon as the packet is sent. */
	CHECK(exchange(PLAN_REPLY, NULL, 0, 0) == 0);
	CHECK(test_radio.TASKS_RXEN == 0);

	/* Silence, a reply finishing after the deadline, and a corrupt reply. */
	memset(rx, 0, sizeof(rx));
	CHECK(exchange(PLAN_SILENT, rx, sizeof(rx), 1200) == RADIO_RX_NONE);
	CHECK(rx[0] == 0);
	CHECK(exchange(PLAN_LATE_REPLY, rx, sizeof(rx), 1200) == RADIO_RX_OK);
	CHECK(exchange(PLAN_BAD_CRC, rx, sizeof(rx), 1200) == RADIO_RX_BAD_CRC);
	CHECK(exchange(PLAN_REPLY, rx, 5, 1200) == RADIO_RX_INVALID);

	/* Bluetooth took the radio: the link defers. */
	CHECK(exchange(PLAN_BLOCKED, rx, sizeof(rx), 1200) == -EAGAIN);
	request_error = -EINVAL;
	CHECK(exchange(PLAN_REPLY, rx, sizeof(rx), 1200) == -EAGAIN);
	request_error = 0;

	/* A slot that never arrives is abandoned and must not run a later job. */
	int before = requests;

	CHECK(exchange(PLAN_NEVER, rx, sizeof(rx), 1200) == -EAGAIN);
	CHECK(exchange(PLAN_REPLY, rx, sizeof(rx), 1200) == -EBUSY);
	CHECK(requests == before + 1);
	send_signal(MPSL_TIMESLOT_SIGNAL_START);
	CHECK(last_action == MPSL_TIMESLOT_SIGNAL_ACTION_END);
	CHECK(exchange(PLAN_REPLY, rx, sizeof(rx), 1200) == RADIO_RX_OK);

	/* The same, resolved by a cancellation instead of a start. */
	CHECK(exchange(PLAN_NEVER, rx, sizeof(rx), 1200) == -EAGAIN);
	send_signal(MPSL_TIMESLOT_SIGNAL_CANCELLED);
	CHECK(exchange(PLAN_REPLY, rx, sizeof(rx), 1200) == RADIO_RX_OK);

	puts("radio timeslot tests passed");
	return 0;
}
