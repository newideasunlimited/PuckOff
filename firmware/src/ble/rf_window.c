/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 * Pure timing/state decisions shared by the MPSL callback and host tests.
 */
#include "rf_window.h"

uint32_t rf_window_elapsed_us(uint32_t start_ticks, uint32_t now_ticks)
{
	uint32_t ticks = (now_ticks - start_ticks) & RF_WINDOW_RTC_MASK;

	return (uint32_t)(((uint64_t)ticks * 15625U + 511U) / 512U) + 31U;
}

uint32_t rf_window_request_timeout(uint32_t budget_us, uint32_t elapsed_us)
{
	if (budget_us > RADIO_MAX_EXCHANGE_US || elapsed_us >= budget_us ||
	    budget_us - elapsed_us <= RF_WINDOW_LENGTH_US + RF_WINDOW_CALL_MARGIN_US) {
		return 0;
	}
	return budget_us - elapsed_us - RF_WINDOW_LENGTH_US - RF_WINDOW_CALL_MARGIN_US;
}

uint32_t rf_window_cutoff(uint32_t slot_now_us, uint32_t budget_us, uint32_t elapsed_us)
{
	if (budget_us > RADIO_MAX_EXCHANGE_US || slot_now_us >= RF_WINDOW_CUTOFF_US ||
	    elapsed_us >= budget_us ||
	    budget_us - elapsed_us <= RF_WINDOW_CALL_MARGIN_US) {
		return 0;
	}
	uint32_t deadline = slot_now_us + budget_us - elapsed_us - RF_WINDOW_CALL_MARGIN_US;

	return deadline < RF_WINDOW_CUTOFF_US ? deadline : RF_WINDOW_CUTOFF_US;
}

uint32_t rf_window_tx_time(uint8_t payload_len)
{
	/* 2 Mbit/s: 8-bit preamble, 5-byte address, 8-bit length, 3-bit S1,
	 * payload and 16-bit CRC; 40 us fast TX ramp, rounding half a bit up.
	 */
	return 40U + (8U + 40U + 8U + 3U + (uint32_t)payload_len * 8U + 16U + 1U) / 2U;
}

bool rf_window_tx_fits(const struct rf_window *window, uint32_t now_us, uint8_t payload_len)
{
	return now_us < window->cutoff_us &&
	       rf_window_tx_time(payload_len) + window->rx_timeout_us +
	       RF_WINDOW_SWITCH_MARGIN_US <= window->cutoff_us - now_us;
}

int rf_window_validate(uint8_t channel, const struct radio_addr *addr,
		       const uint8_t *tx, size_t tx_len, const uint8_t *rx,
		       size_t rx_capacity, uint32_t rx_timeout_us, uint32_t budget_us)
{
	if (!addr || !tx || tx_len < 3 || tx_len > RADIO_PACKET_SIZE ||
	    tx[0] < 1 || tx[0] > RADIO_MAX_PAYLOAD || (size_t)tx[0] + 2 != tx_len ||
	    tx[1] > 7 || channel > 100 || rx_timeout_us > RADIO_MAX_RX_TIMEOUT_US ||
	    budget_us > RADIO_MAX_EXCHANGE_US ||
	    (rx_timeout_us && (!rx || rx_capacity < 3)) ||
	    (!rx_timeout_us && (rx || rx_capacity))) {
		return -EINVAL;
	}
	return 0;
}

enum rf_window_action rf_window_begin(struct rf_window *window, uint32_t cutoff_us,
				     uint32_t now_us, uint8_t payload_len,
				     uint32_t rx_timeout_us, size_t rx_capacity)
{
	*window = (struct rf_window){
		.phase = RF_WINDOW_TX,
		.cutoff_us = cutoff_us,
		.rx_timeout_us = rx_timeout_us,
		.rx_capacity = rx_capacity,
	};
	if (!rf_window_tx_fits(window, now_us, payload_len)) {
		window->phase = RF_WINDOW_DONE;
		window->result = -EAGAIN;
		return RF_WINDOW_FINISH;
	}
	return RF_WINDOW_TRANSMIT;
}

enum rf_window_action rf_window_disabled(struct rf_window *window, uint32_t now_us,
					bool packet_end, bool crc_ok, uint8_t payload_len)
{
	switch (window->phase) {
	case RF_WINDOW_TX:
		if (!packet_end) {
			window->result = -EIO;
		} else if (window->rx_timeout_us) {
			if (now_us >= window->cutoff_us ||
			    window->rx_timeout_us + RF_WINDOW_SWITCH_MARGIN_US >
				    window->cutoff_us - now_us) {
				window->result = -EAGAIN;
			} else {
				window->phase = RF_WINDOW_RX;
				window->rx_deadline_us = now_us + window->rx_timeout_us;
				return RF_WINDOW_RECEIVE;
			}
		}
		break;
	case RF_WINDOW_RX:
		if (!packet_end || now_us >= window->rx_deadline_us) {
			window->result = RADIO_RX_NONE;
		} else if (!crc_ok) {
			window->result = RADIO_RX_BAD_CRC;
		} else if (payload_len < 1 || payload_len > RADIO_MAX_PAYLOAD ||
			   (size_t)payload_len + 2 > window->rx_capacity) {
			window->result = RADIO_RX_INVALID;
		} else {
			window->result = RADIO_RX_OK;
		}
		break;
	case RF_WINDOW_STOPPING:
		break;
	default:
		return RF_WINDOW_NONE;
	}
	window->phase = RF_WINDOW_DONE;
	return RF_WINDOW_FINISH;
}

enum rf_window_action rf_window_stop(struct rf_window *window, int result)
{
	if (window->phase == RF_WINDOW_DONE || window->phase == RF_WINDOW_WAITING) {
		return RF_WINDOW_NONE;
	}
	window->phase = RF_WINDOW_STOPPING;
	window->result = result;
	return RF_WINDOW_DISABLE;
}

enum rf_window_action rf_window_end_guard(struct rf_window *window, bool disabled)
{
	if (window->phase == RF_WINDOW_DONE || window->phase == RF_WINDOW_WAITING) {
		return RF_WINDOW_NONE;
	}
	window->phase = RF_WINDOW_DONE;
	window->result = -ETIMEDOUT;
	return disabled ? RF_WINDOW_FINISH : RF_WINDOW_RESET_FINISH;
}
