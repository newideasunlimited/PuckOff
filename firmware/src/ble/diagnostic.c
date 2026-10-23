/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "gamepad.h"

#include <errno.h>
#include <stdlib.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <nrfx.h>

LOG_MODULE_REGISTER(ble_diagnostic, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static int command_status(const struct shell *shell, size_t argc, char **argv)
{
	struct ble_gamepad_status status;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ble_gamepad_get_status(&status);
	shell_print(shell, "BLE-only diagnostic; proprietary controller RF is NOT running");
	shell_print(shell, "host=%u encrypted=%u subscribed=%u suspended=%u",
		    status.connected, status.encrypted, status.subscribed, status.suspended);
	shell_print(shell, "reports_queued=%u backpressure=%u failures=%u",
		    status.submitted, status.backpressure, status.failures);
	return 0;
}

static int command_neutral(const struct shell *shell, size_t argc, char **argv)
{
	struct gamepad_report report;
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gamepad_neutral(&report);
	err = ble_gamepad_update(&report);
	if (err) {
		shell_error(shell, "Could not queue neutral report (%d)", err);
	}
	return err;
}

static int command_buttons(const struct shell *shell, size_t argc, char **argv)
{
	struct gamepad_report report;
	char *end;
	unsigned long buttons;
	int err;

	ARG_UNUSED(argc);
	errno = 0;
	buttons = strtoul(argv[1], &end, 16);
	if (errno || argv[1][0] == '\0' || argv[1][0] == '-' || *end || buttons > UINT16_MAX) {
		shell_error(shell, "Expected a 16-bit hexadecimal button mask");
		return -EINVAL;
	}
	gamepad_neutral(&report);
	report.buttons = (uint16_t)buttons;
	err = ble_gamepad_update(&report);
	if (err) {
		shell_error(shell, "Could not queue diagnostic report (%d)", err);
	} else {
		shell_print(shell, "Manual diagnostic input, NOT controller input; use 'ble neutral' to release");
	}
	return err;
}

static int command_forget(const struct shell *shell, size_t argc, char **argv)
{
	int err;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	err = ble_gamepad_forget_host();
	if (err) {
		shell_error(shell, "Could not forget BLE host (%d); disconnect the host first", err);
	} else {
		shell_print(shell, "BLE host bond removed; controller pairing records untouched");
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ble_commands,
	SHELL_CMD_ARG(status, NULL, "Show BLE diagnostic status", command_status, 1, 0),
	SHELL_CMD_ARG(neutral, NULL, "Release all diagnostic controls", command_neutral, 1, 0),
	SHELL_CMD_ARG(buttons, NULL, "Manually set a diagnostic 16-bit button mask (hex)",
		      command_buttons, 2, 0),
	SHELL_CMD_ARG(forget_host, NULL, "Forget the disconnected BLE host bond",
		      command_forget, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(ble, &ble_commands, "BLE-only diagnostic; no controller RF", NULL);

static int command_bootloader(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(shell);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	/* Preserve the default image's Adafruit UF2 software recovery path. */
	NRF_POWER->GPREGRET = 0x57;
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
SHELL_CMD_REGISTER(bootloader, NULL, "Reboot into the UF2 drive", command_bootloader);

int main(void)
{
	int err = ble_gamepad_start();

	if (err) {
		LOG_ERR("BLE diagnostic startup failed (%d)", err);
		return err;
	}
	LOG_WRN("BLE-only diagnostic: no Steam Controller radio, no simulated controller");
	if (gpio_is_ready_dt(&led)) {
		err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_ERR("LED configuration failed (%d)", err);
		}
	}
	return 0;
}
