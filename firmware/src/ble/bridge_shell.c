/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "gamepad.h"

#include <zephyr/shell/shell.h>

static int command_status(const struct shell *shell, size_t argc, char **argv)
{
	struct ble_gamepad_status status;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ble_gamepad_get_status(&status);
	shell_print(shell, "host=%u encrypted=%u subscribed=%u suspended=%u",
		    status.connected, status.encrypted, status.subscribed, status.suspended);
	shell_print(shell, "reports_queued=%u backpressure=%u failures=%u",
		    status.submitted, status.backpressure, status.failures);
	return 0;
}

static int command_forget(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = ble_gamepad_forget_host();
	if (err) {
		shell_error(shell, "Could not forget BLE hosts (%d); disconnect the host first", err);
	} else {
		shell_print(shell, "BLE host bonds removed; controller pairing records untouched");
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ble_commands,
	SHELL_CMD_ARG(status, NULL, "Show the Bluetooth host connection", command_status, 1, 0),
	SHELL_CMD_ARG(forget_hosts, NULL, "Forget every disconnected Bluetooth host",
		      command_forget, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ble, &ble_commands, "Bluetooth gamepad", NULL);
