/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "input.h"

/* Generic USB HID gamepad report (no report id). */
struct __packed gamepad_report {
	uint16_t buttons;
	uint8_t hat; /* 0 = up, clockwise to 7; 8 = centered */
	int16_t lx, ly, rx, ry; /* HID convention: +Y is down */
	uint8_t lt, rt;
};

extern const uint8_t gamepad_hid_desc[];
extern const size_t gamepad_hid_desc_len;

void gamepad_from_ctrl(const struct ctrl_state *in, struct gamepad_report *out);
void gamepad_neutral(struct gamepad_report *out);
