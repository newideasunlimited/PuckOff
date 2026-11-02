/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BLE_GAMEPAD_REPORT_SIZE 13

enum ble_report_gate {
	BLE_REPORT_CONNECTED = 1u << 0,
	BLE_REPORT_ENCRYPTED = 1u << 1,
	BLE_REPORT_SUBSCRIBED = 1u << 2,
	BLE_REPORT_SUSPENDED = 1u << 3,
};

/* Protected by the transport's mutex; one newest state, never an input FIFO. */
struct ble_report_state {
	uint8_t report[BLE_GAMEPAD_REPORT_SIZE];
	uint8_t gates;
	bool pending;
};

void ble_report_reset(struct ble_report_state *state);
void ble_report_update(struct ble_report_state *state,
		       const uint8_t report[BLE_GAMEPAD_REPORT_SIZE]);
void ble_report_gate_set(struct ble_report_state *state, enum ble_report_gate gate, bool on);
bool ble_report_ready(const struct ble_report_state *state);
