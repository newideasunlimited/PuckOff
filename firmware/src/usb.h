/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "gamepad.h"

int usb_start(void);
/* Queue a gamepad report for the host. Safe to call from any thread. */
void usb_send_gamepad(const struct gamepad_report *r);
