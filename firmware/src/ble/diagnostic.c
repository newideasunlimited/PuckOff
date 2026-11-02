/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "gamepad.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <nrfx.h>

LOG_MODULE_REGISTER(ble_diagnostic, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

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
