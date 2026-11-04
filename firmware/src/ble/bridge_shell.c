/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "bridge.h"
#include "gamepad.h"
#include "remote.h"
#include "../radio.h"

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
	if (IS_ENABLED(CONFIG_PUCKOFF_BLE_GAMEPAD_ONLY) && mode == BLE_MODE_REMOTE) {
		shell_error(shell, "this image exposes the gamepad only (PUCKOFF_BLE_GAMEPAD_ONLY)");
		return -ENOTSUP;
	}
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

static int command_radio(const struct shell *shell, size_t argc, char **argv)
{
	struct radio_timeslot_diag d;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	radio_timeslot_diag_get(&d);
	shell_print(shell, "requests %u refused %u starts %u blocked %u cancelled %u abandoned %u",
		    d.requests, d.request_refused, d.starts, d.blocked, d.cancelled, d.abandoned);
	shell_print(shell, "signals: radio %u timer %u overstayed %u",
		    d.radio_signals, d.timer_signals, d.overstayed);
	shell_print(shell, "ends: guard %u tx-fail %u rx-none %u rx-ok %u rx-bad %u",
		    d.guard_timeouts, d.tx_failures, d.rx_none, d.rx_ok, d.rx_bad);
	shell_print(shell, "last slot: length %u us, tx done %u, rx deadline %u, finish %u",
		    d.last_length_us, d.last_tx_done_us, d.last_rx_deadline_us, d.last_finish_us);
	shell_print(shell, "last guard: at %u us, phase %u, radio state %u, events 0x%x (1 ready 2 addr 4 end 8 disabled), inten 0x%08x, power %u",
		    d.last_guard_us, d.last_guard_phase, d.last_guard_radio_state,
		    d.last_guard_events, d.last_guard_inten, d.last_guard_power);
	const struct radio_timeslot_hw *h = &d.hw_at_start;

	shell_print(shell, "at start: ppi chen 0x%08x, timer0 cc %u %u %u %u shorts 0x%x inten 0x%08x",
		    h->ppi_chen, h->timer_cc[0], h->timer_cc[1], h->timer_cc[2], h->timer_cc[3],
		    h->timer_shorts, h->timer_inten);
	shell_print(shell, "at start: radio power %u state %u shorts 0x%x inten 0x%08x mode %u modecnf0 0x%x pcnf0 0x%08x pcnf1 0x%08x",
		    h->power, h->state, h->shorts, h->inten, h->mode, h->modecnf0, h->pcnf0,
		    h->pcnf1);
	shell_print(shell, "at start: crccnf 0x%x tifs %u dacnf 0x%x bcc %u rxaddr 0x%x txaddr %u freq %u txpower 0x%x whiteiv 0x%x",
		    h->crccnf, h->tifs, h->dacnf, h->bcc, h->rxaddresses, h->txaddress,
		    h->frequency, h->txpower, h->datawhiteiv);
	shell_print(shell, "at start: trim 173c 0x%08x 1740 0x%08x 1774 0x%08x, ccm enable %u mode 0x%x, aar enable %u",
		    h->reg_173c, h->reg_1740, h->reg_1774, h->ccm_enable, h->ccm_mode,
		    h->aar_enable);
	return 0;
}
SHELL_CMD_REGISTER(radio, NULL, "Shared-radio timeslot diagnostics", command_radio);
