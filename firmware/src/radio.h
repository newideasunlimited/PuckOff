/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* A radio address as stored by the controller: 4 base bytes plus a prefix. */
struct radio_addr {
	uint8_t base[4];
	uint8_t prefix;
};

/* Packet buffer layout used by the hardware: [length][S1][payload...]. */
#define RADIO_MAX_PAYLOAD 96
#define RADIO_PACKET_SIZE (RADIO_MAX_PAYLOAD + 2)
#define RADIO_MAX_RX_TIMEOUT_US 1200U
#define RADIO_MAX_EXCHANGE_US 4000U
/* Raw-backend worst-case TX/disable time plus RTC rounding margin. */
#define RADIO_EXCHANGE_OVERHEAD_US 1100U

enum radio_rx_result {
	RADIO_RX_NONE = 0,
	RADIO_RX_OK = 1,
	RADIO_RX_BAD_CRC = -EBADMSG,
	RADIO_RX_INVALID = -EMSGSIZE,
};

int radio_init(void);

/* Exclusive-owner operation; never call while an exchange is in progress. */
void radio_reset(void);

/*
 * Synchronous, thread-context TX followed by optional RX. Buffer sizes include
 * [length][S1]; TX size must exactly match the length byte. RX timeout includes
 * ramp-up. For TX only, pass NULL/0/0 for RX/capacity/timeout.
 *
 * budget_us covers the entire transaction, including admission and cleanup;
 * backends reject an insufficient budget with -EAGAIN. Maximums are above.
 * Returns RADIO_RX_* or a negative errno for an operational error. RX is
 * unchanged unless the result is RADIO_RX_OK. No caller buffer is retained.
 * Calls must be serialized by the owner; this is not an ISR/MPSL callback API.
 */
int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us);

/* Signal strength of the last received packet, in -dBm. */
uint8_t radio_last_rssi(void);

/* Monotonic RTC time in microseconds (wraps at 2^32; about 31 us resolution). */
uint32_t radio_now_us(void);
