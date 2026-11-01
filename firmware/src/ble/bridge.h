/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The BLE bridge: routes controller input to the connected host in the mode
 * remembered for that host (gamepad or TV remote), and owns the mode chord.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../gamepad.h"
#include "../input.h"
#include "modes.h"

struct ble_bridge_status {
	bool connected;
	uint8_t mode;    /* enum ble_bridge_mode */
	uint8_t profile; /* enum remote_profile */
	uint8_t remembered_hosts;
};

int ble_bridge_start(void);
/* True when a connected, encrypted host took the input; false means use USB. */
bool ble_bridge_input(const struct ctrl_state *in, const struct gamepad_report *report);
/* Controller gone: release everything on the host. True when a host exists. */
bool ble_bridge_lost(void);
/* Set and remember the mode or profile for the connected host. */
int ble_bridge_set_mode(uint8_t mode);
int ble_bridge_set_profile(uint8_t profile);
/* Forget every remembered host mode (with the host bonds). */
int ble_bridge_forget_hosts(void);
void ble_bridge_get_status(struct ble_bridge_status *status);
const char *ble_bridge_mode_name(uint8_t mode);
const char *ble_bridge_profile_name(uint8_t profile);
