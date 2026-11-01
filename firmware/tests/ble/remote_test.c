/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/remote.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t none[BLE_KEYBOARD_REPORT_SIZE];

static int key_count(const struct remote_report *r)
{
	int n = 0;

	for (int i = 2; i < BLE_KEYBOARD_REPORT_SIZE; i++) {
		n += r->keyboard[i] != 0;
	}
	return n;
}

static bool has_key(const struct remote_report *r, uint8_t key)
{
	for (int i = 2; i < BLE_KEYBOARD_REPORT_SIZE; i++) {
		if (r->keyboard[i] == key) {
			return true;
		}
	}
	return false;
}

static bool has_consumer(const struct remote_report *r, uint16_t usage)
{
	for (int i = 0; i < 2; i++) {
		if (r->consumer[2 * i] == (uint8_t)usage && r->consumer[2 * i + 1] == (usage >> 8)) {
			return true;
		}
	}
	return false;
}

int main(void)
{
	struct remote_state st;
	struct remote_report out;
	struct ctrl_state in;

	memset(&in, 0, sizeof(in));
	remote_reset(&st);

	/* Idle controller: every report released. */
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(memcmp(out.keyboard, none, sizeof(none)) == 0);
	assert(out.mouse_buttons == 0 && out.dx == 0 && out.dy == 0 && out.wheel == 0);
	assert(out.consumer[0] == 0 && out.consumer[1] == 0 && out.consumer[3] == 0);

	/* Navigation keys from the d-pad and the left stick; A is Enter. */
	in.buttons = BTN_DUP | BTN_DLEFT | BTN_A;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_key(&out, 0x52) && has_key(&out, 0x50) && has_key(&out, 0x28));
	assert(key_count(&out) == 3 && out.keyboard[0] == 0);
	in.buttons = 0;
	in.lx = 25000;
	in.ly = -25000;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_key(&out, 0x4F) && has_key(&out, 0x51) && key_count(&out) == 2);
	in.lx = 15000;
	in.ly = 0;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(key_count(&out) == 0);
	in.lx = 0;

	/* B: AC Back for TVs, Escape for Apple. */
	in.buttons = BTN_B;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_consumer(&out, 0x0224) && key_count(&out) == 0);
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_APPLE, &out);
	assert(has_key(&out, 0x29) && !has_consumer(&out, 0x0224));

	/* Consumer controls: Steam is Home, triggers are volume, X play/pause. */
	in.buttons = BTN_STEAM | BTN_X;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_consumer(&out, 0x0223) && has_consumer(&out, 0x00CD));
	in.buttons = 0;
	in.lt = 16384;
	in.rt = 16383;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_consumer(&out, 0x00EA) && !has_consumer(&out, 0x00E9));
	in.lt = 0;
	in.buttons = BTN_RT_CLICK;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_consumer(&out, 0x00E9));
	/* Volume wins the two consumer slots over everything else. */
	in.buttons = BTN_RT_CLICK | BTN_LT_CLICK | BTN_STEAM | BTN_Y;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(has_consumer(&out, 0x00E9) && has_consumer(&out, 0x00EA));
	assert(!has_consumer(&out, 0x0223) && !has_consumer(&out, 0x00E2));
	in.buttons = 0;
	in.rt = 0;

	/* Pointer: the first touch sample sets a reference and moves nothing. */
	in.buttons = BTN_RPAD_TOUCH;
	in.rpad_x = 1000;
	in.rpad_y = 1000;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 0 && out.dy == 0);
	/* Then motion scales by 64 counts per pixel, carrying the remainder. */
	in.rpad_x = 1000 + 100;
	in.rpad_y = 1000 - 64;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 1 && out.dy == 1);
	in.rpad_x += 28;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 1 && out.dy == 0);
	/* Lifting and touching elsewhere does not jump. */
	in.buttons = 0;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	in.buttons = BTN_RPAD_TOUCH;
	in.rpad_x = -20000;
	in.rpad_y = 20000;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 0 && out.dy == 0);
	in.rpad_x = -20000 - 640;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == -10 && out.dy == 0);
	/* Pad click is the left button, left pad click the right button. */
	in.buttons = BTN_RPAD_TOUCH | BTN_RPAD_CLICK | BTN_LPAD_CLICK;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.mouse_buttons == 0x03 && out.dx == 0);
	in.buttons = 0;

	/* Right stick also moves the pointer, outside its deadzone. */
	in.rx = 5000;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 0);
	in.rx = 32767;
	in.ry = 32767;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.dx == 7 && out.dy == -7);
	in.rx = 0;
	in.ry = 0;

	/* Left pad vertical motion scrolls, one notch per 2048 units. */
	in.buttons = BTN_LPAD_TOUCH;
	in.lpad_y = 0;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.wheel == 0);
	in.lpad_y = 2048 * 3 + 100;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.wheel == 3);
	in.lpad_y += 2000;
	remote_from_ctrl(&st, &in, REMOTE_PROFILE_ANDROID, &out);
	assert(out.wheel == 1);
	in.buttons = 0;

	/* Chord: Steam + Menu for two seconds fires exactly once per hold. */
	struct remote_chord chord;

	memset(&chord, 0, sizeof(chord));
	assert(!remote_chord_step(&chord, BTN_STEAM, 0));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 1000));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 2999));
	assert(remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 3000));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 9000));
	assert(!remote_chord_step(&chord, BTN_MENU, 9001));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU | BTN_A, 9002));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 11001));
	assert(remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 11002));
	/* Millisecond rollover inside a hold still counts. */
	memset(&chord, 0, sizeof(chord));
	assert(!remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 0xFFFFFF00u));
	assert(remote_chord_step(&chord, BTN_STEAM | BTN_MENU, 0x00000800u));

	puts("remote: keys, consumer controls, pointer accumulation, scroll and chord");
	return 0;
}
