/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/rf_window.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_budget(void)
{
	assert(rf_window_elapsed_us(0, 0) == 31);
	assert(rf_window_elapsed_us(0, 1) == 62);
	assert(rf_window_elapsed_us(0xfffffe, 1) == 123);
	assert(rf_window_request_timeout(4000, 31) == 1719);
	assert(rf_window_request_timeout(2250, 0) == 0);
	assert(rf_window_request_timeout(2251, 0) == 1);
	assert(rf_window_request_timeout(4000, 4001) == 0);
	assert(rf_window_request_timeout(UINT32_MAX, 0) == 0);
	assert(rf_window_cutoff(0, 4000, 31) == 1800);
	assert(rf_window_cutoff(10, 4000, 3000) == 760);
	assert(rf_window_cutoff(1800, 4000, 0) == 0);
	assert(rf_window_cutoff(10, 4000, 4000) == 0);
	assert(rf_window_tx_time(96) == 462);
	assert(rf_window_tx_time(18) == 150);
	assert(rf_window_tx_time(1) == 82);
}

static void test_validation(void)
{
	struct radio_addr addr = {0};
	uint8_t tx[RADIO_PACKET_SIZE] = { 1, 0, 0xe3 };
	uint8_t rx[RADIO_PACKET_SIZE];

	assert(rf_window_validate(18, &addr, tx, 3, rx, sizeof(rx), 1200, 4000) == 0);
	assert(rf_window_validate(18, &addr, tx, 3, NULL, 0, 0, 4000) == 0);
	assert(rf_window_validate(101, &addr, tx, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, NULL, tx, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, NULL, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 2, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 4, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 3, rx, 2, 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 3, NULL, 0, 1200, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 3, rx, sizeof(rx), 0, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 3, rx, sizeof(rx), 1201, 4000) == -EINVAL);
	assert(rf_window_validate(18, &addr, tx, 3, rx, sizeof(rx), 1200, 4001) == -EINVAL);
	tx[1] = 8;
	assert(rf_window_validate(18, &addr, tx, 3, rx, sizeof(rx), 1200, 4000) == -EINVAL);
	tx[1] = 7;
	tx[0] = RADIO_MAX_PAYLOAD;
	assert(rf_window_validate(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) == 0);
	tx[0]++;
	assert(rf_window_validate(18, &addr, tx, sizeof(tx), rx, sizeof(rx), 1200, 4000) ==
	       -EINVAL);
}

static void begin_rx(struct rf_window *window, size_t capacity)
{
	assert(rf_window_begin(window, 1800, 10, 1, 1200, capacity) == RF_WINDOW_TRANSMIT);
	assert(rf_window_disabled(window, 100, true, false, 0) == RF_WINDOW_RECEIVE);
	assert(window->phase == RF_WINDOW_RX);
	assert(window->rx_deadline_us == 1300);
}

static void test_transitions(void)
{
	struct rf_window window;

	assert(rf_window_begin(&window, 1800, 100, 96, 1200, 98) == RF_WINDOW_TRANSMIT);
	assert(rf_window_tx_fits(&window, 108, 96));
	assert(!rf_window_tx_fits(&window, 109, 96));
	assert(rf_window_disabled(&window, 570, true, false, 0) == RF_WINDOW_RECEIVE);
	assert(rf_window_disabled(&window, 1769, true, true, 96) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_OK);
	assert(rf_window_end_guard(&window, false) == RF_WINDOW_NONE);

	assert(rf_window_begin(&window, 1800, 109, 96, 1200, 98) == RF_WINDOW_FINISH);
	assert(window.result == -EAGAIN);
	assert(rf_window_begin(&window, 1800, 10, 18, 0, 0) == RF_WINDOW_TRANSMIT);
	assert(rf_window_disabled(&window, 200, true, false, 0) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_NONE);
	assert(rf_window_begin(&window, 1800, 10, 1, 1200, 98) == RF_WINDOW_TRANSMIT);
	assert(rf_window_disabled(&window, 571, true, false, 0) == RF_WINDOW_FINISH);
	assert(window.result == -EAGAIN);
	assert(rf_window_begin(&window, 1800, 10, 1, 0, 0) == RF_WINDOW_TRANSMIT);
	assert(rf_window_disabled(&window, 100, false, false, 0) == RF_WINDOW_FINISH);
	assert(window.result == -EIO);

	begin_rx(&window, 98);
	assert(rf_window_disabled(&window, 1200, true, false, 96) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_BAD_CRC);
	begin_rx(&window, 97);
	assert(rf_window_disabled(&window, 1200, true, true, 96) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_INVALID);
	begin_rx(&window, 98);
	assert(rf_window_disabled(&window, 1200, true, true, 0) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_INVALID);
	begin_rx(&window, 98);
	assert(rf_window_disabled(&window, 1200, true, true, 97) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_INVALID);
	begin_rx(&window, 98);
	assert(rf_window_disabled(&window, 1300, true, true, 3) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_NONE);

	begin_rx(&window, 98);
	assert(rf_window_stop(&window, RADIO_RX_NONE) == RF_WINDOW_DISABLE);
	assert(rf_window_disabled(&window, 1310, true, true, 3) == RF_WINDOW_FINISH);
	assert(window.result == RADIO_RX_NONE);
	begin_rx(&window, 98);
	assert(rf_window_stop(&window, -ETIMEDOUT) == RF_WINDOW_DISABLE);
	assert(rf_window_disabled(&window, 1810, false, false, 0) == RF_WINDOW_FINISH);
	assert(window.result == -ETIMEDOUT);
	begin_rx(&window, 98);
	assert(rf_window_end_guard(&window, true) == RF_WINDOW_FINISH);
	assert(window.result == -ETIMEDOUT);
	begin_rx(&window, 98);
	assert(rf_window_end_guard(&window, false) == RF_WINDOW_RESET_FINISH);
	assert(window.result == -ETIMEDOUT);
}

static void test_cutoff_sweep(void)
{
	struct rf_window window;

	for (uint32_t elapsed = 0; elapsed <= 4500; ++elapsed) {
		uint32_t cutoff = rf_window_cutoff(20, 4000, elapsed);

		assert(cutoff <= RF_WINDOW_CUTOFF_US);
		if (cutoff) {
			assert(cutoff + RF_WINDOW_CLEANUP_US <= 1900);
			assert(elapsed + cutoff - 20 + RF_WINDOW_CLEANUP_US <= 3850);
		}
		for (uint32_t payload = 1; payload <= 96; ++payload) {
			enum rf_window_action action = rf_window_begin(&window, cutoff, 20,
								       payload, 1200, 98);

			if (action == RF_WINDOW_TRANSMIT) {
				assert(20 + rf_window_tx_time(payload) + 1200 +
				       RF_WINDOW_SWITCH_MARGIN_US <= cutoff);
			} else {
				assert(action == RF_WINDOW_FINISH && window.result == -EAGAIN);
			}
		}
	}
}

int main(void)
{
	test_budget();
	test_validation();
	test_transitions();
	test_cutoff_sweep();
	puts("MPSL RF policy: validation, airtime, absolute budget, IRQ transitions passed");
	return 0;
}
