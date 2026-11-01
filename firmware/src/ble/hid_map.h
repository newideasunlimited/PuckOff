/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Report IDs in the composite map; index into enum ble_report_id is ID - 1. */
#define BLE_HID_REPORT_ID_GAMEPAD 1
#define BLE_HID_REPORT_ID_KEYBOARD 2
#define BLE_HID_REPORT_ID_MOUSE 3
#define BLE_HID_REPORT_ID_CONSUMER 4

extern const uint8_t ble_hid_map[];
extern const size_t ble_hid_map_len;
/* Byte offset where the keyboard collection starts (the gamepad ends before it). */
extern const size_t ble_hid_map_gamepad_len;
