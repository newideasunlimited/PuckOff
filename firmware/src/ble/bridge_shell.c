/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "bridge.h"
#include "gamepad.h"
#include "remote.h"

#include <string.h>
#include <zephyr/shell/shell.h>

static int command_status(const struct shell *shell, size_t argc, char **argv)
{
	struct ble_gamepad_status status;
	struct ble_bridge_status bridge;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ble_gamepad_get_status(&status);
	ble_bridge_get_status(&bridge);
	shell_print(shell, "host=%u encrypted=%u subscribed=%u suspended=%u",
		    status.connected, status.encrypted, status.subscribed, status.suspended);
	shell_print(shell, "reports_queued=%u backpressure=%u failures=%u",
		    status.submitted, status.backpressure, status.failures);
	shell_print(shell, "mode=%s profile=%s remembered_hosts=%u",
		    ble_bridge_mode_name(bridge.mode), ble_bridge_profile_name(bridge.profile),
		    bridge.remembered_hosts);
	return 0;
}

static int command_forget(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = ble_bridge_forget_hosts();
	if (err) {
		shell_error(shell, "Could not forget BLE hosts (%d); disconnect the host first", err);
	} else {
		shell_print(shell, "BLE host bonds and modes removed; controller pairing untouched");
	}
	return err;
}

static int report_result(const struct shell *shell, int err, const char *what)
{
	if (err == -ENOTCONN) {
		shell_error(shell, "connect the Bluetooth host first; the %s is remembered per host",
			    what);
	} else if (err) {
		shell_error(shell, "%s not changed (%d)", what, err);
	} else {
		shell_print(shell, "%s set for this host", what);
	}
	return err;
}

static int command_mode(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t mode;

	if (argc != 2 || (strcmp(argv[1], "gamepad") != 0 && strcmp(argv[1], "remote") != 0)) {
		shell_error(shell, "use: ble mode gamepad|remote");
		return -EINVAL;
	}
	mode = strcmp(argv[1], "remote") == 0 ? BLE_MODE_REMOTE : BLE_MODE_GAMEPAD;
	return report_result(shell, ble_bridge_set_mode(mode), "mode");
}

static int command_profile(const struct shell *shell, size_t argc, char **argv)
{
	uint8_t profile;

	if (argc != 2 || (strcmp(argv[1], "android") != 0 && strcmp(argv[1], "apple") != 0)) {
		shell_error(shell, "use: ble remote android|apple");
		return -EINVAL;
	}
	profile = strcmp(argv[1], "apple") == 0 ? REMOTE_PROFILE_APPLE : REMOTE_PROFILE_ANDROID;
	return report_result(shell, ble_bridge_set_profile(profile), "remote profile");
}

SHELL_STATIC_SUBCMD_SET_CREATE(ble_commands,
	SHELL_CMD_ARG(status, NULL, "Show the Bluetooth host connection and mode", command_status,
		      1, 0),
	SHELL_CMD_ARG(mode, NULL, "mode gamepad|remote: what this host receives", command_mode,
		      2, 0),
	SHELL_CMD_ARG(remote, NULL, "remote android|apple: which key B sends", command_profile,
		      2, 0),
	SHELL_CMD_ARG(forget_hosts, NULL, "Forget every disconnected Bluetooth host",
		      command_forget, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ble, &ble_commands, "Bluetooth gamepad / TV remote", NULL);
