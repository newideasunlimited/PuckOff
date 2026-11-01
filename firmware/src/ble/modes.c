/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "modes.h"
#include "remote.h"

#include <string.h>

static bool used(const struct ble_host_mode *entry)
{
	static const uint8_t none[BLE_PEER_LEN];

	return memcmp(entry->peer, none, BLE_PEER_LEN) != 0;
}

void ble_modes_clear(struct ble_mode_table *table)
{
	memset(table, 0, sizeof(*table));
}

static int find(const struct ble_mode_table *table, const uint8_t peer[BLE_PEER_LEN])
{
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		if (used(&table->host[i]) && memcmp(table->host[i].peer, peer, BLE_PEER_LEN) == 0) {
			return i;
		}
	}
	return -1;
}

void ble_modes_lookup(const struct ble_mode_table *table, const uint8_t peer[BLE_PEER_LEN],
		      uint8_t *mode, uint8_t *profile)
{
	int i = find(table, peer);

	*mode = BLE_MODE_GAMEPAD;
	*profile = REMOTE_PROFILE_ANDROID;
	if (i >= 0) {
		*mode = table->host[i].mode;
		*profile = table->host[i].profile;
	}
}

bool ble_modes_store(struct ble_mode_table *table, const uint8_t peer[BLE_PEER_LEN],
		     uint8_t mode, uint8_t profile)
{
	struct ble_host_mode entry;
	int i;

	if (mode >= BLE_MODE_COUNT || profile >= REMOTE_PROFILE_COUNT) {
		return false;
	}
	memcpy(entry.peer, peer, BLE_PEER_LEN);
	entry.mode = mode;
	entry.profile = profile;
	if (!used(&entry)) {
		return false;
	}
	i = find(table, peer);
	if (i < 0) {
		/* The last slot is the oldest: everything shifts down one. */
		for (i = BLE_MODE_HOSTS - 1; i > 0; i--) {
			table->host[i] = table->host[i - 1];
		}
		i = 0;
	}
	table->host[i] = entry;
	return true;
}

void ble_modes_encode(const struct ble_mode_table *table, uint8_t out[BLE_MODE_TABLE_BYTES])
{
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		uint8_t *p = out + i * (BLE_PEER_LEN + 2);

		memcpy(p, table->host[i].peer, BLE_PEER_LEN);
		p[BLE_PEER_LEN] = table->host[i].mode;
		p[BLE_PEER_LEN + 1] = table->host[i].profile;
	}
}

bool ble_modes_decode(struct ble_mode_table *table, const uint8_t *in, size_t len)
{
	struct ble_mode_table parsed;

	if (len != BLE_MODE_TABLE_BYTES) {
		return false;
	}
	ble_modes_clear(&parsed);
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		const uint8_t *p = in + i * (BLE_PEER_LEN + 2);

		memcpy(parsed.host[i].peer, p, BLE_PEER_LEN);
		parsed.host[i].mode = p[BLE_PEER_LEN];
		parsed.host[i].profile = p[BLE_PEER_LEN + 1];
		if (parsed.host[i].mode >= BLE_MODE_COUNT ||
		    parsed.host[i].profile >= REMOTE_PROFILE_COUNT) {
			return false;
		}
	}
	*table = parsed;
	return true;
}
