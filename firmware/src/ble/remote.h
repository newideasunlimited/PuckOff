/* SPDX-License-Identifier: Apache-2.0 */
/*
 * TV remote mode: the controller drives a BLE keyboard, mouse and consumer
 * control instead of a gamepad. Pure functions; see docs/bluetooth.md.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../input.h"
#include "report_state.h"

/* Which key the B button sends: TVs want AC Back, Apple keyboards use Escape. */
enum remote_profile {
	REMOTE_PROFILE_ANDROID = 0,
	REMOTE_PROFILE_APPLE = 1,
	REMOTE_PROFILE_COUNT,
};

struct remote_report {
	uint8_t keyboard[BLE_KEYBOARD_REPORT_SIZE]; /* modifiers, reserved, 6 key codes */
	uint8_t mouse_buttons;                      /* bit 0 left, bit 1 right */
	int dx, dy, wheel;                          /* relative, already scaled */
	uint8_t consumer[BLE_CONSUMER_REPORT_SIZE]; /* two little-endian usages */
};

/* Pointer tracking between samples of one controller. */
struct remote_state {
	bool rpad_down;
	int16_t rpad_x, rpad_y;
	int32_t rem_x, rem_y;
	bool lpad_down;
	int16_t lpad_y;
	int32_t rem_wheel;
};

/* Steam + Menu held together toggles the mode, once per hold. */
struct remote_chord {
	bool held;
	bool fired;
	uint32_t since_ms;
};

#define REMOTE_CHORD_BUTTONS (BTN_STEAM | BTN_MENU)
#define REMOTE_CHORD_HOLD_MS 2000u

void remote_reset(struct remote_state *state);
void remote_neutral(struct remote_report *out);
void remote_from_ctrl(struct remote_state *state, const struct ctrl_state *in,
		      enum remote_profile profile, struct remote_report *out);
bool remote_chord_step(struct remote_chord *chord, uint32_t buttons, uint32_t now_ms);
