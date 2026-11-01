/* SPDX-License-Identifier: Apache-2.0 */
/* Generic HID: 16 buttons, a hat, two sticks and two analog triggers. */
#include "gamepad.h"
#include <string.h>
#include <zephyr/usb/class/hid.h>

_Static_assert(sizeof(struct gamepad_report) == GAMEPAD_REPORT_SIZE, "gamepad layout");

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
	0x45, 0x00, /* Physical range 0..0: subsequent axes use their logical range. */

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

static const struct {
	uint32_t ctrl;
	uint16_t hid;
} button_map[] = {
	{ BTN_A, GAMEPAD_A },       { BTN_B, GAMEPAD_B },
	{ BTN_X, GAMEPAD_X },       { BTN_Y, GAMEPAD_Y },
	{ BTN_LB, GAMEPAD_LB },     { BTN_RB, GAMEPAD_RB },
	{ BTN_VIEW, GAMEPAD_VIEW }, { BTN_MENU, GAMEPAD_MENU },
	{ BTN_L3, GAMEPAD_L3 },     { BTN_R3, GAMEPAD_R3 },
	{ BTN_STEAM, GAMEPAD_HOME }, { BTN_QAM, GAMEPAD_QAM },
	{ BTN_L4, GAMEPAD_L4 },     { BTN_R4, GAMEPAD_R4 },
	{ BTN_L5, GAMEPAD_L5 },     { BTN_R5, GAMEPAD_R5 },
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
	for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
		if (in->buttons & button_map[i].ctrl) {
			out->buttons |= button_map[i].hid;
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

static void put16(uint8_t *out, uint16_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

void gamepad_encode_generic(const struct gamepad_report *in,
			    uint8_t out[GAMEPAD_REPORT_SIZE])
{
	put16(out, in->buttons);
	out[2] = in->hat;
	put16(out + 3, (uint16_t)in->lx);
	put16(out + 5, (uint16_t)in->ly);
	put16(out + 7, (uint16_t)in->rx);
	put16(out + 9, (uint16_t)in->ry);
	out[11] = in->lt;
	out[12] = in->rt;
}
