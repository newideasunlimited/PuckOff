/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Per-host memory of the bridge mode: a TV wants the remote, a phone wants
 * the gamepad. Pure table logic; the transport supplies peer addresses.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_MODE_HOSTS 4
#define BLE_PEER_LEN 7 /* address type plus six address bytes */

enum ble_bridge_mode {
	BLE_MODE_GAMEPAD = 0,
	BLE_MODE_REMOTE = 1,
	BLE_MODE_COUNT,
};

struct ble_host_mode {
	uint8_t peer[BLE_PEER_LEN]; /* all zero: unused entry */
	uint8_t mode;               /* enum ble_bridge_mode */
	uint8_t profile;            /* enum remote_profile */
};

struct ble_mode_table {
	struct ble_host_mode host[BLE_MODE_HOSTS];
};

#define BLE_MODE_TABLE_BYTES (BLE_MODE_HOSTS * (BLE_PEER_LEN + 2))

void ble_modes_clear(struct ble_mode_table *table);
/* Unknown hosts get the gamepad with the Android profile. */
void ble_modes_lookup(const struct ble_mode_table *table, const uint8_t peer[BLE_PEER_LEN],
		      uint8_t *mode, uint8_t *profile);
/* Remember a host, replacing the oldest entry when the table is full. */
bool ble_modes_store(struct ble_mode_table *table, const uint8_t peer[BLE_PEER_LEN],
		     uint8_t mode, uint8_t profile);
/* Fixed little-endian layout for settings storage. */
void ble_modes_encode(const struct ble_mode_table *table, uint8_t out[BLE_MODE_TABLE_BYTES]);
bool ble_modes_decode(struct ble_mode_table *table, const uint8_t *in, size_t len);
