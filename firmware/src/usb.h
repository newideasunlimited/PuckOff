/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "gamepad.h"

int usb_start(void);
/*
 * Queue the latest state without blocking. Safe to call from any thread.
 * Intermediate states may coalesce; a final state is retried on submit failure.
 * Identical states are muted unless the host requests a nonzero HID idle rate.
 */
void usb_send_gamepad(const struct gamepad_report *r);
