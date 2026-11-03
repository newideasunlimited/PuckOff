/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
/* Independent implementation of the wired report format cited in docs/usb-modes.md. */
#include "usb_switch.h"

#include <zephyr/usb/class/hid.h>

const uint8_t switch_hid_desc[] = {
	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_GAMEPAD),
	HID_COLLECTION(HID_COLLECTION_APPLICATION),

	HID_USAGE_PAGE(HID_USAGE_GEN_BUTTON),
	HID_USAGE_MIN8(1),
	HID_USAGE_MAX8(13),
	HID_LOGICAL_MIN8(0),
	HID_LOGICAL_MAX8(1),
	0x35, 0x00, /* Physical minimum */
	0x45, 0x01, /* Physical maximum */
	HID_REPORT_SIZE(1),
	HID_REPORT_COUNT(13),
	HID_INPUT(0x02),
	HID_REPORT_COUNT(3),
	HID_INPUT(0x01),

	HID_USAGE_PAGE(HID_USAGE_GEN_DESKTOP),
	HID_USAGE(0x39),
	HID_LOGICAL_MAX8(7),
	0x46, 0x3B, 0x01,
	0x65, 0x14,
	HID_REPORT_SIZE(4),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x42),
	0x65, 0x00,
	HID_INPUT(0x01),

	HID_LOGICAL_MAX16(0xFF, 0x00),
	0x46, 0xFF, 0x00,
	HID_USAGE(HID_USAGE_GEN_DESKTOP_X),
	HID_USAGE(HID_USAGE_GEN_DESKTOP_Y),
	HID_USAGE(0x32),
	HID_USAGE(0x35),
	HID_REPORT_SIZE(8),
	HID_REPORT_COUNT(4),
	HID_INPUT(0x02),

	0x06, 0x00, 0xFF, /* Vendor-defined page */
	HID_USAGE(0x20),
	HID_REPORT_COUNT(1),
	HID_INPUT(0x02),
	0x0A, 0x21, 0x26,
	HID_REPORT_COUNT(SWITCH_OUTPUT_SIZE),
	HID_OUTPUT(0x02),
	HID_END_COLLECTION,
};
const size_t switch_hid_desc_len = sizeof(switch_hid_desc);

static uint8_t axis8(int32_t value)
{
	return (uint8_t)((value + 32768) >> 8);
}

static int32_t isqrt32(int64_t n)
{
	int64_t lo = 0, hi = 65536;

	while (lo < hi) {
		int64_t mid = (lo + hi + 1) / 2;

		if (mid * mid <= n) {
			lo = mid;
		} else {
			hi = mid - 1;
		}
	}
	return (int32_t)lo;
}

/*
 * The console applies no deadzone of its own to a wired generic controller, so
 * a stick resting a few percent off centre shows as drift. Deflections inside
 * SWITCH_STICK_DEADZONE report centre; beyond it the vector is rescaled so the
 * deadzone edge maps to centre and full travel still reaches the report edge.
 */
static void stick8(int16_t x, int16_t y, uint8_t *out_x, uint8_t *out_y)
{
	int64_t sx = x < -32767 ? -32767 : x, sy = y < -32767 ? -32767 : y;
	int32_t r = isqrt32(sx * sx + sy * sy);

	if (r <= SWITCH_STICK_DEADZONE) {
		*out_x = 128;
		*out_y = 128;
		return;
	}
	sx = sx * (r - SWITCH_STICK_DEADZONE) * 32767 / ((32767 - SWITCH_STICK_DEADZONE) * (int64_t)r);
	sy = sy * (r - SWITCH_STICK_DEADZONE) * 32767 / ((32767 - SWITCH_STICK_DEADZONE) * (int64_t)r);
	sx = sx > 32767 ? 32767 : (sx < -32767 ? -32767 : sx);
	sy = sy > 32767 ? 32767 : (sy < -32767 ? -32767 : sy);
	*out_x = axis8((int32_t)sx);
	*out_y = axis8((int32_t)sy);
}

void switch_encode_report(const struct gamepad_report *in,
			  uint8_t out[SWITCH_REPORT_SIZE])
{
	/* Preserve face-button positions, not the Steam controller's printed letters. */
	static const uint16_t buttons[] = {
		GAMEPAD_X, GAMEPAD_A, GAMEPAD_B, GAMEPAD_Y,
		GAMEPAD_LB, GAMEPAD_RB, 0, 0,
		GAMEPAD_VIEW, GAMEPAD_MENU, GAMEPAD_L3, GAMEPAD_R3, GAMEPAD_HOME,
	};
	uint16_t bits = 0;

	for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
		if (in->buttons & buttons[i]) {
			bits |= (uint16_t)(1u << i);
		}
	}
	if (in->lt >= 128) {
		bits |= 1u << 6;
	}
	if (in->rt >= 128) {
		bits |= 1u << 7;
	}

	out[0] = (uint8_t)bits;
	out[1] = (uint8_t)(bits >> 8);
	out[2] = in->hat;
	stick8(in->lx, in->ly, &out[3], &out[4]);
	stick8(in->rx, in->ry, &out[5], &out[6]);
	out[7] = 0;
}
