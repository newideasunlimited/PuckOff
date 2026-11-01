/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/modes.h"
#include "../../src/ble/remote.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct ble_mode_table table, copy;
	uint8_t tv[BLE_PEER_LEN] = { 1, 0xAA, 1, 2, 3, 4, 5 };
	uint8_t phone[BLE_PEER_LEN] = { 0, 0xBB, 1, 2, 3, 4, 5 };
	uint8_t zero[BLE_PEER_LEN] = { 0 };
	uint8_t buf[BLE_MODE_TABLE_BYTES];
	uint8_t mode, profile;

	ble_modes_clear(&table);
	ble_modes_lookup(&table, tv, &mode, &profile);
	assert(mode == BLE_MODE_GAMEPAD && profile == REMOTE_PROFILE_ANDROID);

	assert(ble_modes_store(&table, tv, BLE_MODE_REMOTE, REMOTE_PROFILE_APPLE));
	ble_modes_lookup(&table, tv, &mode, &profile);
	assert(mode == BLE_MODE_REMOTE && profile == REMOTE_PROFILE_APPLE);
	ble_modes_lookup(&table, phone, &mode, &profile);
	assert(mode == BLE_MODE_GAMEPAD);

	/* Invalid values and the all-zero peer are refused. */
	assert(!ble_modes_store(&table, tv, BLE_MODE_COUNT, 0));
	assert(!ble_modes_store(&table, tv, 0, REMOTE_PROFILE_COUNT));
	assert(!ble_modes_store(&table, zero, BLE_MODE_REMOTE, 0));

	/* Updating a known host keeps one entry; new hosts push the oldest out. */
	assert(ble_modes_store(&table, tv, BLE_MODE_GAMEPAD, REMOTE_PROFILE_ANDROID));
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		uint8_t peer[BLE_PEER_LEN] = { 1, 0x10 + i, 0, 0, 0, 0, 1 };

		assert(ble_modes_store(&table, peer, BLE_MODE_REMOTE, REMOTE_PROFILE_ANDROID));
	}
	ble_modes_lookup(&table, tv, &mode, &profile);
	assert(mode == BLE_MODE_GAMEPAD); /* evicted: back to the default */
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		uint8_t peer[BLE_PEER_LEN] = { 1, 0x10 + i, 0, 0, 0, 0, 1 };

		ble_modes_lookup(&table, peer, &mode, &profile);
		assert(mode == BLE_MODE_REMOTE);
	}

	/* Storage round trip, and rejection of corrupt records. */
	ble_modes_encode(&table, buf);
	assert(ble_modes_decode(&copy, buf, sizeof(buf)));
	assert(memcmp(&copy, &table, sizeof(table)) == 0);
	assert(!ble_modes_decode(&copy, buf, sizeof(buf) - 1));
	buf[BLE_PEER_LEN] = 7;
	assert(!ble_modes_decode(&copy, buf, sizeof(buf)));
	assert(memcmp(&copy, &table, sizeof(table)) == 0);

	puts("modes: per-host lookup, eviction order, validation and storage layout");
	return 0;
}
