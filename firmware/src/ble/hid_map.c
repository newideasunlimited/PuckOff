/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Composite BLE HID report map: the USB gamepad report under Report ID 1,
 * plus a keyboard, a relative mouse and a consumer control for TV remote
 * mode. Written as plain bytes so host tests can compile it without Zephyr;
 * a test checks the gamepad part against the USB descriptor.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "hid_map.h"

#define GAMEPAD_MAP \
	0x05, 0x01,       /* Usage Page (Generic Desktop) */ \
	0x09, 0x05,       /* Usage (Gamepad) */ \
	0xA1, 0x01,       /* Collection (Application) */ \
	0x85, 0x01,       /*   Report ID (1) */ \
	0x05, 0x09,       /*   Usage Page (Button) */ \
	0x19, 0x01,       /*   Usage Minimum (1) */ \
	0x29, 0x10,       /*   Usage Maximum (16) */ \
	0x15, 0x00,       /*   Logical Minimum (0) */ \
	0x25, 0x01,       /*   Logical Maximum (1) */ \
	0x75, 0x01,       /*   Report Size (1) */ \
	0x95, 0x10,       /*   Report Count (16) */ \
	0x81, 0x02,       /*   Input (Data, Variable, Absolute) */ \
	0x05, 0x01,       /*   Usage Page (Generic Desktop) */ \
	0x09, 0x39,       /*   Usage (Hat Switch) */ \
	0x15, 0x00,       /*   Logical Minimum (0) */ \
	0x25, 0x07,       /*   Logical Maximum (7) */ \
	0x35, 0x00,       /*   Physical Minimum (0) */ \
	0x46, 0x3B, 0x01, /*   Physical Maximum (315) */ \
	0x65, 0x14,       /*   Unit (degrees) */ \
	0x75, 0x04,       /*   Report Size (4) */ \
	0x95, 0x01,       /*   Report Count (1) */ \
	0x81, 0x42,       /*   Input (Data, Variable, Absolute, Null State) */ \
	0x65, 0x00,       /*   Unit (none) */ \
	0x95, 0x01,       /*   Report Count (1) */ \
	0x81, 0x03,       /*   Input (Constant): 4 bits padding */ \
	0x45, 0x00,       /*   Physical Maximum (0): axes use their logical range */ \
	0x09, 0x30,       /*   Usage (X) */ \
	0x09, 0x31,       /*   Usage (Y) */ \
	0x09, 0x32,       /*   Usage (Z) */ \
	0x09, 0x35,       /*   Usage (Rz) */ \
	0x16, 0x01, 0x80, /*   Logical Minimum (-32767) */ \
	0x26, 0xFF, 0x7F, /*   Logical Maximum (32767) */ \
	0x75, 0x10,       /*   Report Size (16) */ \
	0x95, 0x04,       /*   Report Count (4) */ \
	0x81, 0x02,       /*   Input (Data, Variable, Absolute) */ \
	0x09, 0x33,       /*   Usage (Rx): left trigger */ \
	0x09, 0x34,       /*   Usage (Ry): right trigger */ \
	0x15, 0x00,       /*   Logical Minimum (0) */ \
	0x26, 0xFF, 0x00, /*   Logical Maximum (255) */ \
	0x75, 0x08,       /*   Report Size (8) */ \
	0x95, 0x02,       /*   Report Count (2) */ \
	0x81, 0x02,       /*   Input (Data, Variable, Absolute) */ \
	0xC0              /* End Collection */

const uint8_t ble_hid_map[] = {
	GAMEPAD_MAP,

	0x05, 0x01,       /* Usage Page (Generic Desktop) */
	0x09, 0x06,       /* Usage (Keyboard) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, 0x02,       /*   Report ID (2) */
	0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
	0x19, 0xE0,       /*   Usage Minimum (Left Control) */
	0x29, 0xE7,       /*   Usage Maximum (Right GUI) */
	0x15, 0x00,       /*   Logical Minimum (0) */
	0x25, 0x01,       /*   Logical Maximum (1) */
	0x75, 0x01,       /*   Report Size (1) */
	0x95, 0x08,       /*   Report Count (8): modifiers */
	0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
	0x95, 0x01,       /*   Report Count (1) */
	0x75, 0x08,       /*   Report Size (8) */
	0x81, 0x03,       /*   Input (Constant): reserved byte */
	0x95, 0x06,       /*   Report Count (6) */
	0x75, 0x08,       /*   Report Size (8) */
	0x15, 0x00,       /*   Logical Minimum (0) */
	0x26, 0xFF, 0x00, /*   Logical Maximum (255) */
	0x05, 0x07,       /*   Usage Page (Keyboard/Keypad) */
	0x19, 0x00,       /*   Usage Minimum (0) */
	0x2A, 0xFF, 0x00, /*   Usage Maximum (255) */
	0x81, 0x00,       /*   Input (Data, Array): six key codes */
	0xC0,             /* End Collection */

	0x05, 0x01,       /* Usage Page (Generic Desktop) */
	0x09, 0x02,       /* Usage (Mouse) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, 0x03,       /*   Report ID (3) */
	0x09, 0x01,       /*   Usage (Pointer) */
	0xA1, 0x00,       /*   Collection (Physical) */
	0x05, 0x09,       /*     Usage Page (Button) */
	0x19, 0x01,       /*     Usage Minimum (1) */
	0x29, 0x03,       /*     Usage Maximum (3) */
	0x15, 0x00,       /*     Logical Minimum (0) */
	0x25, 0x01,       /*     Logical Maximum (1) */
	0x95, 0x03,       /*     Report Count (3) */
	0x75, 0x01,       /*     Report Size (1) */
	0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
	0x95, 0x01,       /*     Report Count (1) */
	0x75, 0x05,       /*     Report Size (5) */
	0x81, 0x03,       /*     Input (Constant): padding */
	0x05, 0x01,       /*     Usage Page (Generic Desktop) */
	0x09, 0x30,       /*     Usage (X) */
	0x09, 0x31,       /*     Usage (Y) */
	0x09, 0x38,       /*     Usage (Wheel) */
	0x15, 0x81,       /*     Logical Minimum (-127) */
	0x25, 0x7F,       /*     Logical Maximum (127) */
	0x75, 0x08,       /*     Report Size (8) */
	0x95, 0x03,       /*     Report Count (3) */
	0x81, 0x06,       /*     Input (Data, Variable, Relative) */
	0xC0,             /*   End Collection */
	0xC0,             /* End Collection */

	0x05, 0x0C,       /* Usage Page (Consumer) */
	0x09, 0x01,       /* Usage (Consumer Control) */
	0xA1, 0x01,       /* Collection (Application) */
	0x85, 0x04,       /*   Report ID (4) */
	0x15, 0x00,       /*   Logical Minimum (0) */
	0x26, 0xFF, 0x03, /*   Logical Maximum (1023) */
	0x19, 0x00,       /*   Usage Minimum (0) */
	0x2A, 0xFF, 0x03, /*   Usage Maximum (1023) */
	0x75, 0x10,       /*   Report Size (16) */
	0x95, 0x02,       /*   Report Count (2) */
	0x81, 0x00,       /*   Input (Data, Array): two usages */
	0xC0,             /* End Collection */
};
const size_t ble_hid_map_len = sizeof(ble_hid_map);
const size_t ble_hid_map_gamepad_len = sizeof((const uint8_t[]){ GAMEPAD_MAP });
