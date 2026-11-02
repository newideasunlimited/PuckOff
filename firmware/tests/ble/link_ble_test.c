/* SPDX-License-Identifier: Apache-2.0 */
#include "../../../tests/receiver/test_support.h"
#include "../../src/link.c"

static struct bond configured;
static unsigned calls, discovery_calls, beacon_calls, poll_calls;
static bool denied;

int bond_snapshot(int slot, struct bond *out)
{
	*out = slot == 0 ? configured : (struct bond){0};
	return 0;
}
int radio_init(void) { return 0; }
uint32_t radio_now_us(void) { return (uint32_t)test_time_us; }
uint8_t radio_last_rssi(void) { return 0; }
void radio_reset(void) { CHECK(false); }

int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us)
{
	(void)addr;
	CHECK(tx_len == (size_t)tx[0] + 2 && budget_us <= 4000);
	CHECK(budget_us >= POLL_BUDGET_US);
	calls++;
	if (tx[2] == PKT_ADVERTISE) {
		CHECK(tx_len == 20);
		if (channel == RENDEZVOUS_CHANNEL) {
			discovery_calls++;
			CHECK(rx && rx_capacity == RADIO_PACKET_SIZE && rx_timeout_us == 800);
		} else {
			beacon_calls++;
			CHECK(channel == SESSION_CHANNEL && !rx && !rx_capacity && !rx_timeout_us);
		}
	} else {
		CHECK(tx[2] == PKT_POLL && channel == SESSION_CHANNEL);
		poll_calls++;
		CHECK(rx && rx_capacity == RADIO_PACKET_SIZE && rx_timeout_us == 1200);
	}
	test_time_us += 300;
	return denied ? -EAGAIN : RADIO_RX_NONE;
}

static void frame(void)
{
	test_time_us += (uint32_t)(next_poll_us - (uint32_t)test_time_us);
	unsigned before = calls;

	link_step();
	CHECK(calls - before <= 1);
}

int main(void)
{
	configured = (struct bond){ .used = true, .receiver_id = 1, .controller_id = 2 };
	test_time_us = UINT32_MAX - 1000U;
	next_poll_us = (uint32_t)test_time_us;

	frame();
	CHECK(discovery_calls == 1 && beacon_calls == 0 && poll_calls == 0);
	frame();
	CHECK(discovery_calls == 1 && beacon_calls == 1 && poll_calls == 0);
	frame();
	CHECK(poll_calls == 1);
	for (unsigned i = 0; i < 100; ++i) {
		frame();
	}
	CHECK(discovery_calls > 1 && beacon_calls > 1 && poll_calls > 60);

	slots[0].advertised = false;
	slots[0].beaconed = false;
	denied = true;
	unsigned before = poll_calls;

	frame();
	CHECK(!slots[0].advertised && poll_calls == before);
	CHECK(slots[0].stats.deferred > 0);
	denied = false;
	frame();
	CHECK(slots[0].advertised && !slots[0].beaconed);
	frame();
	CHECK(slots[0].beaconed && poll_calls == before);
	frame();
	CHECK(poll_calls == before + 1);
	puts("BLE scheduler: maintenance fairness, no retry bursts, denial and wraparound passed");
	return 0;
}
