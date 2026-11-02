/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/kernel.h>
#include "../gamepad.h"

struct ble_gamepad_status {
	bool connected;
	bool encrypted;
	bool subscribed;
	bool suspended;
	uint32_t submitted;
	uint32_t backpressure;
	uint32_t failures;
};

int ble_gamepad_start(void);

/* Thread context only; copies the report. Never call from an MPSL signal. */
int ble_gamepad_update(const struct gamepad_report *report);
void ble_gamepad_get_status(struct ble_gamepad_status *status);

/* Explicit local action; fails while connected and affects only BLE host keys. */
int ble_gamepad_forget_host(void);
