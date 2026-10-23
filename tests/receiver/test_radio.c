/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include "../../firmware/src/radio.c"

NRF_RADIO_Type test_radio;
static struct onoff_manager manager;
static bool no_manager, clock_pending, tx_stuck, disable_stuck, receive_stuck;
static int request_error, clock_result, cancellations, tx_count;
static int received_length;
static bool good_crc;

struct onoff_manager *z_nrf_clock_control_get_onoff(int clock)
{
	CHECK(clock == CLOCK_CONTROL_NRF_SUBSYS_HF);
	return no_manager ? NULL : &manager;
}
void sys_notify_init_spinwait(struct sys_notify *notify) { (void)notify; }
int onoff_request(struct onoff_manager *mgr, struct onoff_client *client)
{
	CHECK(mgr == &manager && client->node == 0);
	return request_error;
}
int sys_notify_fetch_result(struct sys_notify *notify, int *result)
{
	(void)notify;
	if (clock_pending) { return -EAGAIN; }
	*result = clock_result;
	return 0;
}
int onoff_cancel_or_release(struct onoff_manager *mgr, struct onoff_client *client)
{
	(void)mgr; (void)client; cancellations++; return 0;
}

static void tick(void)
{
	test_time_us += 31;
	/* Model POWER cycling discarding tasks and returning to DISABLED. */
	if (test_radio.PACKETPTR == 0 && test_radio.INTENCLR == UINT32_MAX) {
		test_radio.STATE = 0;
		test_radio.TASKS_TXEN = test_radio.TASKS_RXEN = test_radio.TASKS_DISABLE = 0;
	}
	if (test_radio.TASKS_TXEN) {
		test_radio.TASKS_TXEN = 0;
		tx_count++;
		CHECK(test_radio.PACKETPTR == (uint32_t)(uintptr_t)tx_scratch);
		CHECK(((uintptr_t)tx_scratch & 3) == 0);
		test_radio.STATE = tx_stuck ? 1 : 0;
		test_radio.EVENTS_END = tx_stuck ? 0 : 1;
		test_radio.EVENTS_DISABLED = tx_stuck ? 0 : 1;
	}
	if (test_radio.TASKS_RXEN) {
		test_radio.TASKS_RXEN = 0;
		CHECK(test_radio.PACKETPTR == (uint32_t)(uintptr_t)rx_scratch);
		CHECK(((uintptr_t)rx_scratch & 3) == 0);
		test_radio.STATE = 1;
		if (received_length >= 0) {
			memset(rx_scratch, 0, sizeof(rx_scratch));
			rx_scratch[0] = (uint8_t)received_length;
			rx_scratch[2] = 0xF3;
			test_radio.CRCSTATUS = good_crc;
			test_radio.RSSISAMPLE = 47;
			test_radio.EVENTS_END = 1;
			test_radio.EVENTS_DISABLED = receive_stuck ? 0 : 1;
			test_radio.STATE = receive_stuck ? 1 : 0;
		}
	}
	if (test_radio.TASKS_DISABLE) {
		test_radio.TASKS_DISABLE = 0;
		if (!disable_stuck) {
			test_radio.STATE = 0;
			test_radio.EVENTS_DISABLED = 1;
		}
	}
}

static void reset(void)
{
	memset(&test_radio, 0, sizeof(test_radio));
	test_time_us = 0;
	test_clock_hook = tick;
	initialized = false;
	no_manager = clock_pending = tx_stuck = disable_stuck = receive_stuck = false;
	request_error = clock_result = cancellations = tx_count = 0;
	received_length = 1;
	good_crc = true;
}

static const struct radio_addr address = {{0x01, 0x02, 0x04, 0x08}, 0x10};
static const uint8_t poll[] = {1, 1, 0xE3};

static int exchange(uint8_t *rx, size_t capacity)
{
	return radio_exchange(18, &address, poll, sizeof(poll), rx, capacity, 1200, 4000);
}

static void startup(void)
{
	reset();
	no_manager = true;
	CHECK(radio_init() == -ENODEV);
	no_manager = false; request_error = -ENXIO;
	CHECK(radio_init() == -ENXIO);
	request_error = 0; clock_result = -EIO;
	CHECK(radio_init() == -EIO && !initialized);
	clock_result = 0; clock_pending = true;
	CHECK(radio_init() == -ETIMEDOUT && !initialized);
	CHECK(cancellations == 1 && test_time_us == 100000);
	clock_pending = false;
	CHECK(radio_init() == 0 && initialized);
	CHECK(radio_init() == 0);
}

static void bounds_and_ownership(void)
{
	reset();
	uint8_t rx[RADIO_PACKET_SIZE];
	memset(rx, 0xA5, sizeof(rx));
	CHECK(exchange(rx, sizeof(rx)) == -ENODEV);
	CHECK(radio_init() == 0);
	CHECK(radio_exchange(18, NULL, poll, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &address, NULL, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &address, poll, 2, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	CHECK(radio_exchange(101, &address, poll, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &address, poll, 3, NULL, 0, 1200, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &address, poll, 3, rx, sizeof(rx), 1201, 4000) == -EINVAL);
	CHECK(radio_exchange(18, &address, poll, 3, rx, sizeof(rx), 1200, 4001) == -EINVAL);
	CHECK(radio_exchange(18, &address, poll, 3, rx, sizeof(rx), 1200, 2299) == -EAGAIN);
	CHECK(tx_count == 0);
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_OK && rx[0] == 1 && rx[2] == 0xF3);
	CHECK(radio_last_rssi() == 47 && test_radio.SHORTS == 0);
	CHECK(test_radio.BASE0 == 0x80402010 && test_radio.PREFIX0 == 0x08);
	CHECK(test_radio.PCNF1 % 256 == RADIO_MAX_PAYLOAD);

	uint8_t unaligned[4] = {0, 1, 1, 0xE3};
	CHECK(radio_exchange(18, &address, unaligned + 1, 3, NULL, 0, 0, 1100) == 0);
	CHECK(memcmp(tx_scratch, poll, sizeof(poll)) == 0);
	unaligned[2] = 8;
	CHECK(radio_exchange(18, &address, unaligned + 1, 3, NULL, 0, 0, 1100) == -EINVAL);
}

static void reception_and_recovery(void)
{
	reset();
	CHECK(radio_init() == 0);
	uint8_t rx[RADIO_PACKET_SIZE], before[RADIO_PACKET_SIZE];
	memset(rx, 0xA5, sizeof(rx)); memcpy(before, rx, sizeof(rx));
	received_length = -1;
	uint64_t start = test_time_us;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_NONE);
	CHECK(test_time_us - start < 2300 && test_radio.STATE == 0);
	CHECK(memcmp(rx, before, sizeof(rx)) == 0);
	received_length = 1; good_crc = false;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_BAD_CRC);
	CHECK(memcmp(rx, before, sizeof(rx)) == 0);
	good_crc = true; received_length = 255;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_INVALID);
	received_length = 0;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_INVALID);
	received_length = RADIO_MAX_PAYLOAD;
	CHECK(exchange(rx, 3) == RADIO_RX_INVALID);
	CHECK(memcmp(rx, before, sizeof(rx)) == 0);
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_OK);

	tx_stuck = true;
	start = test_time_us;
	CHECK(exchange(rx, sizeof(rx)) == -ETIMEDOUT);
	CHECK(test_time_us - start < 1100 && test_radio.PACKETPTR == 0);
	CHECK(test_radio.SHORTS == 0 && test_radio.INTENCLR == UINT32_MAX);
	tx_stuck = false; received_length = 1;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_OK); /* recovery, no reboot */
	receive_stuck = disable_stuck = true;
	CHECK(exchange(rx, sizeof(rx)) == -ETIMEDOUT && test_radio.PACKETPTR == 0);
	receive_stuck = disable_stuck = false;
	CHECK(exchange(rx, sizeof(rx)) == RADIO_RX_OK);

	/* A stale DISABLED event must not turn a failed TX into success. */
	test_radio.EVENTS_DISABLED = 1; test_radio.EVENTS_END = 1; tx_stuck = true;
	CHECK(exchange(rx, sizeof(rx)) == -ETIMEDOUT);
}

static void timer_wrap(void)
{
	reset();
	test_clock_hook = NULL;
	test_time_us = (uint64_t)UINT32_MAX - 100;
	uint32_t before = radio_now_us();
	test_time_us += 1000;
	uint32_t after = radio_now_us();
	CHECK(after < before && after - before >= 970 && after - before <= 1031);
	test_time_us = ((uint64_t)UINT32_MAX + 100) * 1000;
	before = radio_now_us();
	test_time_us += 1000;
	CHECK(radio_now_us() - before >= 970 && radio_now_us() - before <= 1031);
}

int main(void)
{
	startup();
	bounds_and_ownership();
	reception_and_recovery();
	timer_wrap();
	puts("radio: clock errors, DMA bounds, CRC, timeouts, reset recovery and timer wrap");
	return 0;
}
