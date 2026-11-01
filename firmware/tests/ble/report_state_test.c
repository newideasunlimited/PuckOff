/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/report_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void make_ready(struct ble_report_state *state)
{
	ble_report_gate_set(state, BLE_REPORT_CONNECTED, true);
	ble_report_gate_set(state, BLE_REPORT_ENCRYPTED, true);
	ble_report_subscribe(state, BLE_REPORT_GAMEPAD, true);
}

static void drain(struct ble_report_state *state)
{
	int id;

	while ((id = ble_report_next(state)) >= 0) {
		ble_report_sent(state, (enum ble_report_id)id);
	}
}

int main(void)
{
	struct ble_report_state state;
	uint8_t report[BLE_GAMEPAD_REPORT_SIZE] = { 0, 0, 8 };
	const uint8_t neutral[BLE_GAMEPAD_REPORT_SIZE] = { 0, 0, 8 };
	const uint8_t zeros[BLE_REPORT_MAX] = { 0 };
	uint8_t keys[BLE_KEYBOARD_REPORT_SIZE] = { 0, 0, 0x28 };

	assert(ble_report_size(BLE_REPORT_GAMEPAD) == 13);
	assert(ble_report_size(BLE_REPORT_KEYBOARD) == 8);
	assert(ble_report_size(BLE_REPORT_MOUSE) == 4);
	assert(ble_report_size(BLE_REPORT_CONSUMER) == 4);
	assert(ble_report_size(BLE_REPORT_COUNT) == 0);

	/* Neutral wire shape: hat centered, everything else released. */
	ble_report_reset(&state);
	assert(memcmp(state.report[BLE_REPORT_GAMEPAD].data, neutral, sizeof(neutral)) == 0);
	assert(memcmp(state.report[BLE_REPORT_KEYBOARD].data, zeros, 8) == 0);
	assert(memcmp(state.report[BLE_REPORT_MOUSE].data, zeros, 4) == 0);
	assert(memcmp(state.report[BLE_REPORT_CONSUMER].data, zeros, 4) == 0);

	/* Gates: nothing goes out until connected, encrypted and subscribed. */
	assert(ble_report_next(&state) < 0);
	ble_report_gate_set(&state, BLE_REPORT_CONNECTED, true);
	assert(ble_report_next(&state) < 0);
	ble_report_subscribe(&state, BLE_REPORT_GAMEPAD, true);
	assert(ble_report_next(&state) < 0);
	ble_report_gate_set(&state, BLE_REPORT_ENCRYPTED, true);
	assert(ble_report_next(&state) == BLE_REPORT_GAMEPAD);
	drain(&state);
	assert(ble_report_next(&state) < 0);

	/* Unsubscribed reports never go out, even when pending. */
	ble_report_update(&state, BLE_REPORT_KEYBOARD, keys);
	assert(state.report[BLE_REPORT_KEYBOARD].pending);
	assert(ble_report_next(&state) < 0);
	ble_report_subscribe(&state, BLE_REPORT_KEYBOARD, true);
	assert(ble_report_next(&state) == BLE_REPORT_KEYBOARD);
	ble_report_sent(&state, BLE_REPORT_KEYBOARD);

	/* Coalescing: 10,000 absolute updates leave one newest pending report. */
	ble_report_update(&state, BLE_REPORT_GAMEPAD, neutral);
	assert(ble_report_next(&state) < 0);
	for (unsigned int i = 1; i <= 10000; ++i) {
		report[0] = (uint8_t)i;
		report[1] = (uint8_t)(i >> 8);
		ble_report_update(&state, BLE_REPORT_GAMEPAD, report);
	}
	assert(ble_report_next(&state) == BLE_REPORT_GAMEPAD);
	assert(memcmp(state.report[BLE_REPORT_GAMEPAD].data, report, sizeof(report)) == 0);
	ble_report_sent(&state, BLE_REPORT_GAMEPAD);
	ble_report_update(&state, BLE_REPORT_GAMEPAD, report);
	assert(ble_report_next(&state) < 0);

	/* Suspend holds reports; resume resends the current state of every report. */
	ble_report_gate_set(&state, BLE_REPORT_SUSPENDED, true);
	ble_report_update(&state, BLE_REPORT_GAMEPAD, neutral);
	assert(ble_report_next(&state) < 0);
	ble_report_gate_set(&state, BLE_REPORT_SUSPENDED, false);
	assert(ble_report_next(&state) == BLE_REPORT_GAMEPAD);
	ble_report_sent(&state, BLE_REPORT_GAMEPAD);
	assert(ble_report_next(&state) == BLE_REPORT_KEYBOARD);
	drain(&state);

	/* Relative mouse motion accumulates and saturates until it is delivered. */
	ble_report_subscribe(&state, BLE_REPORT_MOUSE, true);
	drain(&state);
	ble_report_move(&state, 0, 0, 0, 0);
	assert(ble_report_next(&state) < 0);
	ble_report_move(&state, 0, 3, -2, 0);
	ble_report_move(&state, 0, 3, -2, 1);
	assert(ble_report_next(&state) == BLE_REPORT_MOUSE);
	assert((int8_t)state.report[BLE_REPORT_MOUSE].data[1] == 6);
	assert((int8_t)state.report[BLE_REPORT_MOUSE].data[2] == -4);
	assert((int8_t)state.report[BLE_REPORT_MOUSE].data[3] == 1);
	for (int i = 0; i < 100; i++) {
		ble_report_move(&state, 0, 5, -5, 0);
	}
	assert((int8_t)state.report[BLE_REPORT_MOUSE].data[1] == 127);
	assert((int8_t)state.report[BLE_REPORT_MOUSE].data[2] == -128);
	ble_report_move(&state, 0x01, 0, 0, 0);
	ble_report_sent(&state, BLE_REPORT_MOUSE);
	assert(state.report[BLE_REPORT_MOUSE].data[0] == 0x01);
	assert(memcmp(state.report[BLE_REPORT_MOUSE].data + 1, zeros, 3) == 0);
	assert(ble_report_next(&state) < 0);
	/* The same motion twice is still motion, unlike an unchanged absolute report. */
	ble_report_move(&state, 0x01, 1, 0, 0);
	ble_report_sent(&state, BLE_REPORT_MOUSE);
	ble_report_move(&state, 0x01, 1, 0, 0);
	assert(ble_report_next(&state) == BLE_REPORT_MOUSE);
	/* A button release with no motion is a change. */
	ble_report_sent(&state, BLE_REPORT_MOUSE);
	ble_report_move(&state, 0x00, 0, 0, 0);
	assert(ble_report_next(&state) == BLE_REPORT_MOUSE);
	drain(&state);

	/* Neutral releases everything but keeps the host's subscriptions. */
	ble_report_update(&state, BLE_REPORT_KEYBOARD, keys);
	ble_report_neutral(&state);
	assert(memcmp(state.report[BLE_REPORT_KEYBOARD].data, zeros, 8) == 0);
	assert(state.report[BLE_REPORT_KEYBOARD].subscribed);
	assert(ble_report_next(&state) == BLE_REPORT_GAMEPAD);
	drain(&state);

	/* Disconnect: a new host starts unsubscribed with neutral pending reports. */
	ble_report_gate_set(&state, BLE_REPORT_CONNECTED, false);
	assert(ble_report_next(&state) < 0);
	ble_report_reset(&state);
	assert(!state.report[BLE_REPORT_GAMEPAD].subscribed && state.gates == 0);
	make_ready(&state);
	assert(ble_report_next(&state) == BLE_REPORT_GAMEPAD);

	puts("report_state: gates, per-report subscriptions, coalescing, relative motion");
	return 0;
}
