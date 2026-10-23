/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* A radio address as stored by the controller: 4 base bytes plus a prefix. */
struct radio_addr {
	uint8_t base[4];
	uint8_t prefix;
};

/* Packet buffer layout used by the hardware: [length][S1][payload...]. */
#define RADIO_MAX_PAYLOAD 96

enum radio_rx_result {
	RADIO_RX_NONE = 0,
	RADIO_RX_OK = 1,
	RADIO_RX_BAD_CRC = -1,
};

int radio_init(void);

/* Power-cycle the radio peripheral to recover from a wedged state. */
void radio_reset(void);

/* Transmit one packet and block until it has left the antenna. */
void radio_send(uint8_t channel, const struct radio_addr *addr, const uint8_t *pkt);

/*
 * Listen on the same channel and address used by the last radio_send() for up
 * to timeout_us. Returns as soon as a packet arrives.
 */
enum radio_rx_result radio_listen(uint8_t *pkt, uint32_t timeout_us);

/* Signal strength of the last received packet, in -dBm. */
uint8_t radio_last_rssi(void);

/* Free-running microsecond clock. */
uint32_t radio_now_us(void);
