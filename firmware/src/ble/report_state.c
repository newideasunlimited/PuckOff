/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "report_state.h"

#include <string.h>

#define GAMEPAD_HAT_CENTERED 8
#define MOUSE_BUTTONS 0
#define MOUSE_DX 1
#define MOUSE_DY 2
#define MOUSE_WHEEL 3

static const uint8_t sizes[BLE_REPORT_COUNT] = {
	BLE_GAMEPAD_REPORT_SIZE, BLE_KEYBOARD_REPORT_SIZE,
	BLE_MOUSE_REPORT_SIZE, BLE_CONSUMER_REPORT_SIZE,
};

uint8_t ble_report_size(enum ble_report_id id)
{
	return (unsigned int)id < BLE_REPORT_COUNT ? sizes[id] : 0;
}

static void neutral(struct ble_report_slot *slot, enum ble_report_id id)
{
	memset(slot->data, 0, sizeof(slot->data));
	if (id == BLE_REPORT_GAMEPAD) {
		slot->data[2] = GAMEPAD_HAT_CENTERED;
	}
	slot->pending = true;
}

void ble_report_reset(struct ble_report_state *state)
{
	memset(state, 0, sizeof(*state));
	for (int i = 0; i < BLE_REPORT_COUNT; i++) {
		neutral(&state->report[i], (enum ble_report_id)i);
	}
}

void ble_report_neutral(struct ble_report_state *state)
{
	for (int i = 0; i < BLE_REPORT_COUNT; i++) {
		neutral(&state->report[i], (enum ble_report_id)i);
	}
}

void ble_report_update(struct ble_report_state *state, enum ble_report_id id,
		       const uint8_t *data)
{
	struct ble_report_slot *slot;
	uint8_t len = ble_report_size(id);

	if (!len) {
		return;
	}
	slot = &state->report[id];
	if (memcmp(slot->data, data, len) != 0) {
		memcpy(slot->data, data, len);
		slot->pending = true;
	}
}

static int8_t saturate(int value)
{
	return value > INT8_MAX ? INT8_MAX : value < INT8_MIN ? INT8_MIN : (int8_t)value;
}

void ble_report_move(struct ble_report_state *state, uint8_t buttons, int dx, int dy, int wheel)
{
	struct ble_report_slot *slot = &state->report[BLE_REPORT_MOUSE];
	int8_t x = saturate((int8_t)slot->data[MOUSE_DX] + dx);
	int8_t y = saturate((int8_t)slot->data[MOUSE_DY] + dy);
	int8_t w = saturate((int8_t)slot->data[MOUSE_WHEEL] + wheel);

	if (slot->data[MOUSE_BUTTONS] != buttons || dx || dy || wheel) {
		slot->pending = true;
	}
	slot->data[MOUSE_BUTTONS] = buttons;
	slot->data[MOUSE_DX] = (uint8_t)x;
	slot->data[MOUSE_DY] = (uint8_t)y;
	slot->data[MOUSE_WHEEL] = (uint8_t)w;
}

void ble_report_subscribe(struct ble_report_state *state, enum ble_report_id id, bool on)
{
	if ((unsigned int)id >= BLE_REPORT_COUNT) {
		return;
	}
	state->report[id].subscribed = on;
	/* A host that resubscribes must see the current state even if unchanged. */
	state->report[id].pending = true;
}

void ble_report_gate_set(struct ble_report_state *state, enum ble_report_gate gate, bool on)
{
	if (on) {
		state->gates |= gate;
	} else {
		state->gates &= (uint8_t)~gate;
	}
	/* Exit from suspend or a new encrypted link must resend even unchanged input. */
	for (int i = 0; i < BLE_REPORT_COUNT; i++) {
		state->report[i].pending = true;
	}
}

int ble_report_next(const struct ble_report_state *state)
{
	const uint8_t required = BLE_REPORT_CONNECTED | BLE_REPORT_ENCRYPTED;

	if (state->gates != required) {
		return -1;
	}
	for (int i = 0; i < BLE_REPORT_COUNT; i++) {
		if (state->report[i].pending && state->report[i].subscribed) {
			return i;
		}
	}
	return -1;
}

void ble_report_sent(struct ble_report_state *state, enum ble_report_id id)
{
	struct ble_report_slot *slot;

	if ((unsigned int)id >= BLE_REPORT_COUNT) {
		return;
	}
	slot = &state->report[id];
	slot->pending = false;
	if (id == BLE_REPORT_MOUSE) {
		/* Delivered motion is consumed; buttons are state and stay. */
		slot->data[MOUSE_DX] = 0;
		slot->data[MOUSE_DY] = 0;
		slot->data[MOUSE_WHEEL] = 0;
	}
}
