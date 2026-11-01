/* SPDX-License-Identifier: Apache-2.0 */
/* RADIO register setup shared by the exclusive and timeslot backends. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <hal/nrf_radio.h>
#include <nrfx.h>
#include "radio.h"

static inline uint8_t bitrev8(uint8_t x)
{
	x = (uint8_t)((x & 0xF0) >> 4 | (x & 0x0F) << 4);
	x = (uint8_t)((x & 0xCC) >> 2 | (x & 0x33) << 2);
	x = (uint8_t)((x & 0xAA) >> 1 | (x & 0x55) << 1);
	return x;
}

/* The RADIO must be disabled before calling this. */
static inline void radio_regs_configure(uint8_t channel, const struct radio_addr *addr)
{
	NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_2Mbit << RADIO_MODE_MODE_Pos;
	NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;
	NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->FREQUENCY = channel;

	/* 8-bit length field, 3-bit S1 field (packet id + no-ack flag). */
	NRF_RADIO->PCNF0 = (8U << RADIO_PCNF0_LFLEN_Pos) | (3U << RADIO_PCNF0_S1LEN_Pos);
	/* Big-endian, 4 base bytes + prefix, no whitening. */
	NRF_RADIO->PCNF1 = (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
			   (4U << RADIO_PCNF1_BALEN_Pos) |
			   (RADIO_MAX_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos);

	/* CRC-16-CCITT over address and payload. */
	NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->CRCPOLY = 0x11021;
	NRF_RADIO->CRCINIT = 0xFFFF;

	/* The air format sends each address byte bit-reversed. */
	NRF_RADIO->BASE0 = (uint32_t)bitrev8(addr->base[0]) << 24 |
			   (uint32_t)bitrev8(addr->base[1]) << 16 |
			   (uint32_t)bitrev8(addr->base[2]) << 8 |
			   (uint32_t)bitrev8(addr->base[3]);
	NRF_RADIO->PREFIX0 = bitrev8(addr->prefix);
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->RXADDRESSES = 1;
}

/* Reject what the hardware cannot send or the caller cannot receive. */
static inline bool radio_exchange_args_valid(uint8_t channel, const struct radio_addr *addr,
					     const uint8_t *tx, size_t tx_len, const uint8_t *rx,
					     size_t rx_capacity, uint32_t rx_timeout_us,
					     uint32_t budget_us)
{
	return !(addr == NULL || tx == NULL || tx_len < 3 || tx_len > RADIO_PACKET_SIZE ||
		 tx[0] < 1 || tx[0] > RADIO_MAX_PAYLOAD || (size_t)tx[0] + 2 != tx_len ||
		 tx[1] > 7 || channel > 100 || rx_timeout_us > RADIO_MAX_RX_TIMEOUT_US ||
		 budget_us > RADIO_MAX_EXCHANGE_US ||
		 (rx_timeout_us && (rx == NULL || rx_capacity < 3)) ||
		 (!rx_timeout_us && (rx != NULL || rx_capacity != 0)));
}
