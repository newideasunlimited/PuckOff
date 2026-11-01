/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* One HID input report per report ID; the gamepad is the largest. */
#define BLE_REPORT_MAX 13
#define BLE_GAMEPAD_REPORT_SIZE 13
#define BLE_KEYBOARD_REPORT_SIZE 8
#define BLE_MOUSE_REPORT_SIZE 4
#define BLE_CONSUMER_REPORT_SIZE 4

/* Order matches the Report ID (index + 1) and the report map in hid_map.c. */
enum ble_report_id {
	BLE_REPORT_GAMEPAD,
	BLE_REPORT_KEYBOARD,
	BLE_REPORT_MOUSE,
	BLE_REPORT_CONSUMER,
	BLE_REPORT_COUNT,
};

enum ble_report_gate {
	BLE_REPORT_CONNECTED = 1u << 0,
	BLE_REPORT_ENCRYPTED = 1u << 1,
	BLE_REPORT_SUSPENDED = 1u << 3,
};

struct ble_report_slot {
	uint8_t data[BLE_REPORT_MAX];
	bool pending;
	bool subscribed;
};

/* Protected by the transport's mutex; one newest state per report, never a FIFO. */
struct ble_report_state {
	struct ble_report_slot report[BLE_REPORT_COUNT];
	uint8_t gates;
};

uint8_t ble_report_size(enum ble_report_id id);
/* New host: neutral reports, nothing subscribed, every report pending. */
void ble_report_reset(struct ble_report_state *state);
/* Release everything (mode change, controller lost); subscriptions kept. */
void ble_report_neutral(struct ble_report_state *state);
/* Absolute reports: an unchanged report is not resent. */
void ble_report_update(struct ble_report_state *state, enum ble_report_id id,
		       const uint8_t *data);
/* Relative mouse motion accumulates (saturating) until the report is sent. */
void ble_report_move(struct ble_report_state *state, uint8_t buttons, int dx, int dy, int wheel);
void ble_report_subscribe(struct ble_report_state *state, enum ble_report_id id, bool on);
void ble_report_gate_set(struct ble_report_state *state, enum ble_report_gate gate, bool on);
/* The first report that should go out now, or -1. */
int ble_report_next(const struct ble_report_state *state);
void ble_report_sent(struct ble_report_state *state, enum ble_report_id id);
