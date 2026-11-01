/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "remote.h"

#include <string.h>

/* HID keyboard usage IDs (usage page 0x07). */
#define KEY_ENTER 0x28
#define KEY_ESCAPE 0x29
#define KEY_RIGHT 0x4F
#define KEY_LEFT 0x50
#define KEY_DOWN 0x51
#define KEY_UP 0x52
#define KEYBOARD_KEYS 6
#define KEYBOARD_FIRST_KEY 2

/* HID consumer usage IDs (usage page 0x0C). */
#define CONSUMER_MENU 0x0040
#define CONSUMER_PLAY_PAUSE 0x00CD
#define CONSUMER_SCAN_NEXT 0x00B5
#define CONSUMER_SCAN_PREVIOUS 0x00B6
#define CONSUMER_MUTE 0x00E2
#define CONSUMER_VOLUME_UP 0x00E9
#define CONSUMER_VOLUME_DOWN 0x00EA
#define CONSUMER_AC_SEARCH 0x0221
#define CONSUMER_AC_HOME 0x0223
#define CONSUMER_AC_BACK 0x0224
#define CONSUMER_SLOTS 2

#define MOUSE_LEFT 0x01
#define MOUSE_RIGHT 0x02

/* A stick past this deflection acts as a direction key. */
#define STICK_KEY_THRESHOLD 20000
/* Right stick pointer: counts per report at full deflection. */
#define STICK_POINTER_DEADZONE 6000
#define STICK_POINTER_DIVISOR 4096
/* Trackpad coordinates span the signed 16-bit range; one swipe is about 1000 counts. */
#define PAD_POINTER_DIVISOR 64
/* Left pad vertical motion: one wheel notch per 1/32 of the pad. */
#define PAD_WHEEL_DIVISOR 2048
/* Triggers report 0..32767; half travel or the click counts as pressed. */
#define TRIGGER_THRESHOLD 16384

void remote_reset(struct remote_state *state)
{
	memset(state, 0, sizeof(*state));
}

void remote_neutral(struct remote_report *out)
{
	memset(out, 0, sizeof(*out));
}

static void press_key(struct remote_report *out, uint8_t key)
{
	for (int i = 0; i < KEYBOARD_KEYS; i++) {
		uint8_t *slot = &out->keyboard[KEYBOARD_FIRST_KEY + i];

		if (*slot == key) {
			return;
		}
		if (*slot == 0) {
			*slot = key;
			return;
		}
	}
}

static void press_consumer(struct remote_report *out, uint16_t usage)
{
	for (int i = 0; i < CONSUMER_SLOTS; i++) {
		uint8_t *slot = &out->consumer[2 * i];

		if (slot[0] == (uint8_t)usage && slot[1] == (uint8_t)(usage >> 8)) {
			return;
		}
		if (slot[0] == 0 && slot[1] == 0) {
			slot[0] = (uint8_t)usage;
			slot[1] = (uint8_t)(usage >> 8);
			return;
		}
	}
}

/* Split an accumulated distance into whole counts, carrying the remainder. */
static int take_counts(int32_t *remainder, int32_t delta, int32_t divisor)
{
	int32_t total = *remainder + delta;
	int32_t counts = total / divisor;

	*remainder = total - counts * divisor;
	return counts;
}

static void pointer(struct remote_state *state, const struct ctrl_state *in,
		    struct remote_report *out)
{
	bool down = (in->buttons & BTN_RPAD_TOUCH) != 0;

	if (down && state->rpad_down) {
		/* Pad +Y is up; HID mouse +Y is down. */
		out->dx += take_counts(&state->rem_x, in->rpad_x - state->rpad_x,
				       PAD_POINTER_DIVISOR);
		out->dy += take_counts(&state->rem_y, state->rpad_y - in->rpad_y,
				       PAD_POINTER_DIVISOR);
	} else {
		/* A new touch starts from where the finger is; no jump. */
		state->rem_x = 0;
		state->rem_y = 0;
	}
	state->rpad_down = down;
	state->rpad_x = in->rpad_x;
	state->rpad_y = in->rpad_y;

	if (in->rx > STICK_POINTER_DEADZONE || in->rx < -STICK_POINTER_DEADZONE) {
		out->dx += in->rx / STICK_POINTER_DIVISOR;
	}
	if (in->ry > STICK_POINTER_DEADZONE || in->ry < -STICK_POINTER_DEADZONE) {
		out->dy -= in->ry / STICK_POINTER_DIVISOR;
	}

	down = (in->buttons & BTN_LPAD_TOUCH) != 0;
	if (down && state->lpad_down) {
		out->wheel += take_counts(&state->rem_wheel, in->lpad_y - state->lpad_y,
					  PAD_WHEEL_DIVISOR);
	} else {
		state->rem_wheel = 0;
	}
	state->lpad_down = down;
	state->lpad_y = in->lpad_y;
}

void remote_from_ctrl(struct remote_state *state, const struct ctrl_state *in,
		      enum remote_profile profile, struct remote_report *out)
{
	uint32_t b = in->buttons;

	remote_neutral(out);

	if ((b & BTN_DUP) || in->ly > STICK_KEY_THRESHOLD) {
		press_key(out, KEY_UP);
	}
	if ((b & BTN_DDOWN) || in->ly < -STICK_KEY_THRESHOLD) {
		press_key(out, KEY_DOWN);
	}
	if ((b & BTN_DLEFT) || in->lx < -STICK_KEY_THRESHOLD) {
		press_key(out, KEY_LEFT);
	}
	if ((b & BTN_DRIGHT) || in->lx > STICK_KEY_THRESHOLD) {
		press_key(out, KEY_RIGHT);
	}
	if (b & BTN_A) {
		press_key(out, KEY_ENTER);
	}
	if (b & BTN_B) {
		if (profile == REMOTE_PROFILE_APPLE) {
			press_key(out, KEY_ESCAPE);
		} else {
			press_consumer(out, CONSUMER_AC_BACK);
		}
	}

	/* Volume first: it is the control most likely held together with another. */
	if (in->lt >= TRIGGER_THRESHOLD || (b & BTN_LT_CLICK)) {
		press_consumer(out, CONSUMER_VOLUME_DOWN);
	}
	if (in->rt >= TRIGGER_THRESHOLD || (b & BTN_RT_CLICK)) {
		press_consumer(out, CONSUMER_VOLUME_UP);
	}
	if (b & BTN_STEAM) {
		press_consumer(out, CONSUMER_AC_HOME);
	}
	if (b & BTN_MENU) {
		press_consumer(out, CONSUMER_MENU);
	}
	if (b & BTN_VIEW) {
		press_consumer(out, CONSUMER_AC_SEARCH);
	}
	if (b & BTN_X) {
		press_consumer(out, CONSUMER_PLAY_PAUSE);
	}
	if (b & BTN_Y) {
		press_consumer(out, CONSUMER_MUTE);
	}
	if (b & BTN_RB) {
		press_consumer(out, CONSUMER_SCAN_NEXT);
	}
	if (b & BTN_LB) {
		press_consumer(out, CONSUMER_SCAN_PREVIOUS);
	}

	if (b & BTN_RPAD_CLICK) {
		out->mouse_buttons |= MOUSE_LEFT;
	}
	if (b & BTN_LPAD_CLICK) {
		out->mouse_buttons |= MOUSE_RIGHT;
	}
	pointer(state, in, out);
}

bool remote_chord_step(struct remote_chord *chord, uint32_t buttons, uint32_t now_ms)
{
	bool held = (buttons & REMOTE_CHORD_BUTTONS) == REMOTE_CHORD_BUTTONS;

	if (!held) {
		chord->held = false;
		chord->fired = false;
		return false;
	}
	if (!chord->held) {
		chord->held = true;
		chord->since_ms = now_ms;
		return false;
	}
	if (!chord->fired && now_ms - chord->since_ms >= REMOTE_CHORD_HOLD_MS) {
		chord->fired = true;
		return true;
	}
	return false;
}
