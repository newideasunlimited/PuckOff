/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Generic HID gamepad: 16 buttons, a hat switch, two sticks and two analog
 * triggers. Works on Windows, macOS, Linux, Android, ChromeOS and in browsers
 * without drivers.
 */
#include "gamepad.h"
#include <string.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/hid.h>

const uint8_t gamepad_hid_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_GAMEPAD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),

	/* 16 buttons */
	HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
	HID_USAGE_MIN8(1),
	HID_USAGE_MAX8(16),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(16),
	HID_INPUT(0x02),

	/* D-pad as a hat switch, plus 4 bits padding */
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(0x39),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(7),
	0x35, 0x00,       /* Physical Minimum (0) */
	0x46, 0x3B, 0x01, /* Physical Maximum (315) */
	0x65, 0x14,       /* Unit (degrees) */
	HID_REPORT_SIZE(4),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x42), /* null state = centered */
	0x65, 0x00,       /* Unit (none) */
	HID_REPORT_COUNT(1),
	HID_INPUT(0x03),

	/* Sticks: X/Y left, Z/Rz right */
	HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
	HID_USAGE(0x32),
	HID_USAGE(0x35),
	HID_LOGICAL_MIN16(0x01, 0x80), /* -32767 */
	HID_LOGICAL_MAX16(0xFF, 0x7F), /* 32767 */
	HID_REPORT_SIZE(16),
	HID_REPORT_COUNT(4),
	HID_INPUT(0x02),

	/* Triggers: Rx left, Ry right */
	HID_USAGE(0x33),
	HID_USAGE(0x34),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX16(0xFF, 0x00),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(2),
	HID_INPUT(0x02),

	HID_END_COLLECTION,
};
const size_t gamepad_hid_desc_len = sizeof(gamepad_hid_desc);

/* HID button number (1-based) for each controller button. */
static const struct {
	uint32_t ctrl;
	uint8_t hid;
} button_map[] = {
	{ BTN_A, 1 },     { BTN_B, 2 },     { BTN_X, 3 },      { BTN_Y, 4 },
	{ BTN_LB, 5 },    { BTN_RB, 6 },    { BTN_VIEW, 7 },   { BTN_MENU, 8 },
	{ BTN_L3, 9 },    { BTN_R3, 10 },   { BTN_STEAM, 11 }, { BTN_QAM, 12 },
	{ BTN_L4, 13 },   { BTN_R4, 14 },   { BTN_L5, 15 },    { BTN_R5, 16 },
};

static uint8_t hat_from(uint32_t b)
{
	bool up = b & BTN_DUP, down = b & BTN_DDOWN;
	bool left = b & BTN_DLEFT, right = b & BTN_DRIGHT;

	if (up && down) {
		up = down = false;
	}
	if (left && right) {
		left = right = false;
	}
	if (up) {
		return right ? 1 : left ? 7 : 0;
	}
	if (down) {
		return right ? 3 : left ? 5 : 4;
	}
	if (right) {
		return 2;
	}
	if (left) {
		return 6;
	}
	return 8;
}

static int16_t flip(int16_t v)
{
	return v == INT16_MIN ? INT16_MAX : (int16_t)-v;
}

static uint8_t trigger8(uint16_t v, bool clicked)
{
	uint16_t t = v >> 7;

	if (clicked || t > 255) {
		return 255;
	}
	return (uint8_t)t;
}

void gamepad_neutral(struct gamepad_report *out)
{
	memset(out, 0, sizeof(*out));
	out->hat = 8;
}

void gamepad_from_ctrl(const struct ctrl_state *in, struct gamepad_report *out)
{
	gamepad_neutral(out);
	for (size_t i = 0; i < ARRAY_SIZE(button_map); i++) {
		if (in->buttons & button_map[i].ctrl) {
			out->buttons |= (uint16_t)(1u << (button_map[i].hid - 1));
		}
	}
	out->hat = hat_from(in->buttons);
	out->lx = in->lx == INT16_MIN ? -INT16_MAX : in->lx;
	out->ly = flip(in->ly);
	out->rx = in->rx == INT16_MIN ? -INT16_MAX : in->rx;
	out->ry = flip(in->ry);
	out->lt = trigger8(in->lt, in->buttons & BTN_LT_CLICK);
	out->rt = trigger8(in->rt, in->buttons & BTN_RT_CLICK);
}
