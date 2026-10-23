/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
#pragma once

#include "gamepad.h"

#define SWITCH_REPORT_SIZE 8
#define SWITCH_OUTPUT_SIZE 8

extern const uint8_t switch_hid_desc[];
extern const size_t switch_hid_desc_len;

void switch_encode_report(const struct gamepad_report *in,
			  uint8_t out[SWITCH_REPORT_SIZE]);
