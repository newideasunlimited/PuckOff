/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include <pthread.h>
/* Exercise the production scheduler without starting its infinite RTOS loop. */
#include "../../firmware/src/link.c"

static struct bond saved[BOND_SLOTS];
static int reply[BOND_SLOTS];
static uint32_t poll_cost_us;
static int init_error;
static bool mutate_on_poll;
static unsigned inputs[BOND_SLOTS], losses[BOND_SLOTS], polls[BOND_SLOTS];
static unsigned ads;
static uint64_t poll_times[2048];
static unsigned poll_count;
static atomic_bool reader_ready;

int bond_snapshot(int slot, struct bond *out)
{
	CHECK(slot >= 0 && slot < BOND_SLOTS && out != NULL);
	*out = saved[slot];
	return 0;
}
int radio_init(void) { return init_error; }
uint32_t radio_now_us(void) { return (uint32_t)test_time_us; }
uint8_t radio_last_rssi(void) { return 42; }
void radio_reset(void) { CHECK(false); }

int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us)
{
	CHECK(budget_us <= POLL_PERIOD_US);
	CHECK(tx_len == (size_t)tx[0] + 2 && tx[1] <= 7);
	if (tx[2] == PKT_ADVERTISE) {
		ads++;
		CHECK(tx_len == 20 && tx[11] == SESSION_CHANNEL);
		CHECK(channel == SESSION_CHANNEL || channel == RENDEZVOUS_CHANNEL);
		CHECK(budget_us >= RADIO_EXCHANGE_OVERHEAD_US + rx_timeout_us);
		test_time_us += 100 + rx_timeout_us;
		return RADIO_RX_NONE;
	}
	CHECK(tx[2] == PKT_POLL && channel == SESSION_CHANNEL);
	CHECK(rx != NULL && rx_capacity == RADIO_PACKET_SIZE);
	CHECK(rx_timeout_us == POLL_REPLY_WINDOW_US && budget_us >= POLL_BUDGET_US);
	int slot = -1;
	for (int i = 0; i < BOND_SLOTS; i++) {
		if (saved[i].used && memcmp(addr, &slots[i].session, sizeof(*addr)) == 0) {
			slot = i;
			break;
		}
	}
	CHECK(slot >= 0 && poll_count < 2048);
	polls[slot]++;
	poll_times[poll_count++] = test_time_us;
	test_time_us += poll_cost_us;
	if (mutate_on_poll) {
		saved[slot].receiver_id++;
		mutate_on_poll = false;
	}
	if (reply[slot] <= 0) { return reply[slot]; }
	memset(rx, 0, rx_capacity);
	rx[0] = 1;
	rx[2] = (uint8_t)reply[slot];
	if (reply[slot] == PKT_INPUT) {
		rx[0] = 33; rx[3] = 30; rx[4] = 6; rx[5] = 0x42;
		rx[7] = (uint8_t)(1U << slot);
	}
	return RADIO_RX_OK;
}

static void got_input(int slot, const struct ctrl_state *state)
{
	CHECK(state->buttons == (1U << slot));
	inputs[slot]++;
}
static void lost_input(int slot) { losses[slot]++; }

static void reset_at(uint64_t now)
{
	memset(slots, 0, sizeof(slots));
	memset(saved, 0, sizeof(saved));
	memset(reply, 0, sizeof(reply));
	memset(inputs, 0, sizeof(inputs));
	memset(losses, 0, sizeof(losses));
	memset(polls, 0, sizeof(polls));
	atomic_clear(&started);
	atomic_clear(&verbose);
	active_slot = -1;
	next_slot = 0;
	test_time_us = now;
	next_poll_us = (uint32_t)now;
	input_cb = got_input;
	lost_cb = lost_input;
	poll_cost_us = 200;
	mutate_on_poll = false;
	init_error = 0;
	test_hwinfo_result = 8;
	test_thread_count = 0;
	poll_count = 0;
	ads = 0;
	for (int i = 0; i < BOND_SLOTS; i++) {
		saved[i].receiver_id = 0x11223344U + (uint32_t)i;
		saved[i].controller_id = 0x55667700U + (uint32_t)i;
		memcpy(saved[i].serial, "SYNTHETIC", 10);
	}
}

static void frame(void)
{
	int32_t wait = (int32_t)(next_poll_us - radio_now_us());
	if (wait > 0) { test_time_us += (uint32_t)wait; }
	link_step();
}

static void ownership_and_replacement(void)
{
	reset_at(0);
	saved[0].used = saved[1].used = true;
	reply[0] = reply[1] = PKT_INPUT;
	frame();
	for (int i = 0; i < 10; i++) { frame(); }
	CHECK(active_slot == 0 && inputs[0] == 11 && polls[1] == 0);
	CHECK(link_any_connected());
	struct radio_addr before = slots[0].session;
	saved[0].receiver_id++;
	frame();
	CHECK(losses[0] == 1);
	CHECK(memcmp(&before, &slots[0].session, sizeof(before)) != 0);
	CHECK(slots[0].stats.reports == 1); /* new bond starts fresh */

	saved[0].serial[0] = 'X';
	frame();
	CHECK(losses[0] == 2 && slots[0].stats.reports == 1);
	saved[0].controller_id++;
	frame();
	CHECK(losses[0] == 3);
	saved[0].used = false;
	frame();
	CHECK(losses[0] == 4 && active_slot == 1 && inputs[1] == 1);
	CHECK(!slots[0].stats.connected && slots[0].stats.reports == 0);

	reset_at(0);
	saved[0].used = true;
	reply[0] = PKT_INPUT;
	mutate_on_poll = true;
	frame();
	CHECK(inputs[0] == 0 && active_slot == -1);
	frame();
	CHECK(inputs[0] == 1);
}

static void liveness_and_goodbye(void)
{
	reset_at(0);
	saved[0].used = true; reply[0] = PKT_INPUT;
	frame();
	reply[0] = PKT_STATUS;
	for (int i = 0; i < 80; i++) { frame(); }
	CHECK(losses[0] == 1 && active_slot == -1 && !link_any_connected());
	CHECK(slots[0].stats.replies == 81); /* status cannot keep stale input held */

	reset_at(0);
	saved[0].used = true; reply[0] = PKT_INPUT;
	frame();
	reply[0] = 0xF4;
	for (int i = 0; i < 80; i++) { frame(); }
	CHECK(losses[0] == 1 && slots[0].stats.replies == 1);
	CHECK(slots[0].stats.invalid_packets > 0);

	reset_at(((uint64_t)UINT32_MAX - 100) * 1000);
	saved[0].used = true; reply[0] = PKT_INPUT;
	frame();
	reply[0] = PKT_GOODBYE;
	frame();
	CHECK(losses[0] == 1 && slots[0].stats.powered_off);
	uint64_t stopped = test_time_us;
	unsigned count = polls[0];
	test_time_us = stopped + 2499000;
	link_step();
	CHECK(polls[0] == count);
	test_time_us = stopped + 2501000;
	reply[0] = PKT_INPUT;
	frame();
	CHECK(polls[0] == count + 1 && active_slot == 0);
	CHECK(!slots[0].stats.powered_off);
}

static void timing_and_budgets(void)
{
	reset_at((uint64_t)UINT32_MAX - 1000);
	saved[0].used = true; reply[0] = PKT_INPUT;
	frame(); frame();
	CHECK(poll_times[1] - poll_times[0] == POLL_PERIOD_US);
	test_time_us += 45000;
	unsigned count = poll_count;
	link_step();
	CHECK(poll_count == count + 1 && frame_remaining() > 0);

	reset_at(((uint64_t)INT32_MAX + 100) * 1000);
	saved[0].used = true; reply[0] = PKT_INPUT;
	frame();
	CHECK(inputs[0] == 1); /* no zero-deadline sentinel at high uptime */

	reset_at(0);
	saved[0].used = true; reply[0] = PKT_INPUT;
	test_time_us = 2500;
	link_step();
	CHECK(poll_count == 0 && ads == 0);
	frame();
	CHECK(inputs[0] == 1);
	reply[0] = RADIO_RX_NONE;
	poll_cost_us = 1300;
	count = polls[0];
	frame();
	CHECK(polls[0] == count + 2);
	CHECK(frame_remaining() > 0);
	poll_cost_us = 1900;
	count = polls[0];
	frame();
	CHECK(polls[0] == count + 1);
	reply[0] = -ETIMEDOUT;
	count = polls[0];
	frame();
	CHECK(polls[0] == count + 1 && slots[0].stats.radio_errors == 1);
	reply[0] = -EAGAIN;
	frame();
	CHECK(slots[0].stats.deferred == 1);

	reset_at(0);
	for (int i = 0; i < BOND_SLOTS; i++) { saved[i].used = true; }
	poll_cost_us = 1300;
	for (int i = 0; i < 8; i++) { frame(); }
	for (int i = 0; i < BOND_SLOTS; i++) { CHECK(polls[i] == 2); }
	CHECK(ads <= 8);
}

static void initialization_and_snapshots(void)
{
	reset_at(0);
	struct link_stats st;
	CHECK(link_get_stats(-1, &st) == -EINVAL);
	CHECK(link_get_stats(BOND_SLOTS, &st) == -EINVAL);
	CHECK(link_get_stats(0, NULL) == -EINVAL);
	CHECK(link_get_stats(0, &st) == 0 && !st.connected);
	test_hwinfo_result = -ENODEV;
	CHECK(link_start(got_input, lost_input) == -ENODEV && test_thread_count == 0);
	test_hwinfo_result = 4;
	CHECK(link_start(got_input, lost_input) == -EIO && test_thread_count == 0);
	test_hwinfo_result = 8; init_error = -EIO;
	CHECK(link_start(got_input, lost_input) == -EIO && test_thread_count == 0);
	init_error = 0;
	CHECK(link_start(got_input, lost_input) == 0 && test_thread_count == 1);
	CHECK(link_start(got_input, lost_input) == -EALREADY && test_thread_count == 1);
	link_set_verbose(true);
	CHECK(atomic_get(&verbose));
}

static void *stats_reader(void *unused)
{
	(void)unused;
	atomic_store(&reader_ready, true);
	for (int n = 0; n < 10000; n++) {
		struct link_stats st;

		CHECK(link_get_stats(0, &st) == 0);
		CHECK(st.reports <= st.polls);
		CHECK(!st.connected || st.reports > 0);
		(void)link_any_connected();
		link_set_verbose((n & 1) != 0);
	}
	return NULL;
}

static void concurrent_snapshots(void)
{
	pthread_t reader;

	reset_at(0);
	saved[0].used = true; reply[0] = PKT_INPUT;
	atomic_store(&reader_ready, false);
	CHECK(pthread_create(&reader, NULL, stats_reader, NULL) == 0);
	while (!atomic_load(&reader_ready)) {}
	for (int n = 0; n < 1000; n++) { frame(); }
	CHECK(pthread_join(reader, NULL) == 0);
	CHECK(slots[0].stats.reports == 1000);
}

int main(void)
{
	ownership_and_replacement();
	liveness_and_goodbye();
	timing_and_budgets();
	initialization_and_snapshots();
	concurrent_snapshots();
	puts("link: ownership, rebond/clear, stale input, rollover, scheduling and errors");
	return 0;
}
