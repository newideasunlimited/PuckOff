/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/report_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_ready(struct ble_report_state *state)
{
	ble_report_gate_set(state, BLE_REPORT_CONNECTED, true);
	ble_report_gate_set(state, BLE_REPORT_ENCRYPTED, true);
	ble_report_gate_set(state, BLE_REPORT_SUBSCRIBED, true);
}

int main(void)
{
	struct ble_report_state state;
	uint8_t report[BLE_GAMEPAD_REPORT_SIZE] = { 0, 0, 8 };
	const uint8_t neutral[BLE_GAMEPAD_REPORT_SIZE] = { 0, 0, 8 };

	ble_report_reset(&state);
	assert(sizeof(state.report) == 13);
	assert(memcmp(state.report, neutral, sizeof(neutral)) == 0);
	assert(!ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_CONNECTED, true);
	assert(!ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_SUBSCRIBED, true);
	assert(!ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_ENCRYPTED, true);
	assert(ble_report_ready(&state));

	state.pending = false;
	ble_report_update(&state, neutral);
	assert(!ble_report_ready(&state));
	for (unsigned int i = 1; i <= 10000; ++i) {
		report[0] = (uint8_t)i;
		report[1] = (uint8_t)(i >> 8);
		ble_report_update(&state, report);
	}
	assert(ble_report_ready(&state));
	assert(memcmp(state.report, report, sizeof(report)) == 0);

	ble_report_gate_set(&state, BLE_REPORT_SUSPENDED, true);
	assert(!ble_report_ready(&state));
	ble_report_update(&state, neutral);
	assert(!ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_SUSPENDED, false);
	assert(ble_report_ready(&state));
	assert(memcmp(state.report, neutral, sizeof(neutral)) == 0);

	state.pending = false;
	ble_report_gate_set(&state, BLE_REPORT_SUBSCRIBED, false);
	assert(!ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_SUBSCRIBED, true);
	assert(ble_report_ready(&state));

	ble_report_update(&state, report);
	ble_report_reset(&state);
	assert(!ble_report_ready(&state));
	assert(memcmp(state.report, neutral, sizeof(neutral)) == 0);
	make_ready(&state);
	assert(ble_report_ready(&state));
	ble_report_gate_set(&state, BLE_REPORT_ENCRYPTED, false);
	assert(!ble_report_ready(&state));

	puts("BLE report state: neutral, gates, coalescing, suspend, reconnect passed");
	return 0;
}
