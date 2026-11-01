/* SPDX-License-Identifier: Apache-2.0 */
/* The HID report descriptor item encodings used by firmware/src/gamepad.c. */
#pragma once
#define HID_USAGE_GEN_DESKTOP 0x01
#define HID_USAGE_GEN_BUTTON 0x09
#define HID_USAGE_GEN_DESKTOP_GAMEPAD 0x05
#define HID_USAGE_GEN_DESKTOP_X 0x30
#define HID_USAGE_GEN_DESKTOP_Y 0x31
#define HID_COLLECTION_APPLICATION 0x01
#define HID_USAGE_PAGE(page) 0x05, (page)
#define HID_USAGE(usage) 0x09, (usage)
#define HID_COLLECTION(type) 0xA1, (type)
#define HID_END_COLLECTION 0xC0
#define HID_USAGE_MIN8(v) 0x19, (v)
#define HID_USAGE_MAX8(v) 0x29, (v)
#define HID_LOGICAL_MIN8(v) 0x15, (v)
#define HID_LOGICAL_MAX8(v) 0x25, (v)
#define HID_LOGICAL_MIN16(lo, hi) 0x16, (lo), (hi)
#define HID_LOGICAL_MAX16(lo, hi) 0x26, (lo), (hi)
#define HID_REPORT_SIZE(v) 0x75, (v)
#define HID_REPORT_COUNT(v) 0x95, (v)
#define HID_INPUT(v) 0x81, (v)
