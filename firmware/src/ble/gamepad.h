/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/kernel.h>
#include "../gamepad.h"
#include "modes.h"
#include "remote.h"

struct ble_gamepad_status {
	bool connected;
	bool encrypted;
	bool subscribed; /* the gamepad report */
	bool suspended;
	uint32_t submitted;
	uint32_t backpressure;
	uint32_t failures;
};

int ble_gamepad_start(void);

/* Thread context only; copies the report. Never call from an MPSL signal. */
int ble_gamepad_update(const struct gamepad_report *report);
/* Remote mode: keyboard, mouse and consumer reports from one controller sample. */
int ble_gamepad_update_remote(const struct remote_report *report);
/* Release every report (controller lost, mode change). */
void ble_gamepad_neutral(void);
/* Identity address of the connected host, or false without one. */
bool ble_gamepad_host_peer(uint8_t peer[BLE_PEER_LEN]);
void ble_gamepad_get_status(struct ble_gamepad_status *status);

/* Explicit local action; fails while connected and affects only BLE host keys. */
int ble_gamepad_forget_host(void);
