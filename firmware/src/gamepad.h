/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <zephyr/toolchain.h>
#include "input.h"

#define GAMEPAD_REPORT_SIZE 13

enum gamepad_button {
	GAMEPAD_A = 1u << 0,
	GAMEPAD_B = 1u << 1,
	GAMEPAD_X = 1u << 2,
	GAMEPAD_Y = 1u << 3,
	GAMEPAD_LB = 1u << 4,
	GAMEPAD_RB = 1u << 5,
	GAMEPAD_VIEW = 1u << 6,
	GAMEPAD_MENU = 1u << 7,
	GAMEPAD_L3 = 1u << 8,
	GAMEPAD_R3 = 1u << 9,
	GAMEPAD_HOME = 1u << 10,
	GAMEPAD_QAM = 1u << 11,
	GAMEPAD_L4 = 1u << 12,
	GAMEPAD_R4 = 1u << 13,
	GAMEPAD_L5 = 1u << 14,
	GAMEPAD_R5 = 1u << 15,
};

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
/* Wire encoding is little-endian regardless of the CPU's byte order. */
void gamepad_encode_generic(const struct gamepad_report *in,
			    uint8_t out[GAMEPAD_REPORT_SIZE]);
