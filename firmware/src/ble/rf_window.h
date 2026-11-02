/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include "../radio.h"

#define RF_WINDOW_LENGTH_US 2000U
#define RF_WINDOW_CUTOFF_US 1800U
#define RF_WINDOW_CLEANUP_US 100U
#define RF_WINDOW_CALL_MARGIN_US 250U
#define RF_WINDOW_SWITCH_MARGIN_US 30U
#define RF_WINDOW_PERIOD_US 4000U
#define RF_WINDOW_RTC_MASK 0x00ffffffU

enum rf_window_phase {
	RF_WINDOW_WAITING,
	RF_WINDOW_TX,
	RF_WINDOW_RX,
	RF_WINDOW_STOPPING,
	RF_WINDOW_DONE,
};

enum rf_window_action {
	RF_WINDOW_NONE,
	RF_WINDOW_TRANSMIT,
	RF_WINDOW_RECEIVE,
	RF_WINDOW_DISABLE,
	RF_WINDOW_FINISH,
	RF_WINDOW_RESET_FINISH,
};

struct rf_window {
	enum rf_window_phase phase;
	uint32_t cutoff_us;
	uint32_t rx_timeout_us;
	uint32_t rx_deadline_us;
	size_t rx_capacity;
	int result;
};

/* RTC1 is 24-bit / 32768 Hz. Include one tick of conservative quantization. */
uint32_t rf_window_elapsed_us(uint32_t start_ticks, uint32_t now_ticks);
uint32_t rf_window_request_timeout(uint32_t budget_us, uint32_t elapsed_us);
uint32_t rf_window_cutoff(uint32_t slot_now_us, uint32_t budget_us, uint32_t elapsed_us);
uint32_t rf_window_tx_time(uint8_t payload_len);
bool rf_window_tx_fits(const struct rf_window *window, uint32_t now_us, uint8_t payload_len);

int rf_window_validate(uint8_t channel, const struct radio_addr *addr,
		       const uint8_t *tx, size_t tx_len, const uint8_t *rx,
		       size_t rx_capacity, uint32_t rx_timeout_us, uint32_t budget_us);
enum rf_window_action rf_window_begin(struct rf_window *window, uint32_t cutoff_us,
				     uint32_t now_us, uint8_t payload_len,
				     uint32_t rx_timeout_us, size_t rx_capacity);
enum rf_window_action rf_window_disabled(struct rf_window *window, uint32_t now_us,
					bool packet_end, bool crc_ok, uint8_t payload_len);
enum rf_window_action rf_window_stop(struct rf_window *window, int result);
enum rf_window_action rf_window_end_guard(struct rf_window *window, bool disabled);
