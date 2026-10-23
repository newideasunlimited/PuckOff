/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "report_state.h"

#include <string.h>

void ble_report_reset(struct ble_report_state *state)
{
	memset(state, 0, sizeof(*state));
	state->report[2] = 8;
	state->pending = true;
}

void ble_report_update(struct ble_report_state *state,
		       const uint8_t report[BLE_GAMEPAD_REPORT_SIZE])
{
	if (memcmp(state->report, report, sizeof(state->report)) != 0) {
		memcpy(state->report, report, sizeof(state->report));
		state->pending = true;
	}
}

void ble_report_gate_set(struct ble_report_state *state, enum ble_report_gate gate, bool on)
{
	if (on) {
		state->gates |= gate;
	} else {
		state->gates &= (uint8_t)~gate;
	}
	/* Resubscription and exit-suspend must resend even unchanged input. */
	state->pending = true;
}

bool ble_report_ready(const struct ble_report_state *state)
{
	const uint8_t required = BLE_REPORT_CONNECTED | BLE_REPORT_ENCRYPTED |
				 BLE_REPORT_SUBSCRIBED;

	return state->pending && state->gates == required;
}
