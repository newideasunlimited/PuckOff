/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 *
 * The nRF52840 watchdog cannot be stopped once started and keeps counting
 * through a soft reset, so the firmware never soft-resets with it running.
 * Entering the UF2 drive lets the watchdog perform the reset: a watchdog reset
 * clears the watchdog itself, and the Adafruit bootloader reads the retained
 * GPREGRET magic exactly as it does after the original `bootloader` command.
 *
 * The record lives in the RETAINED region from ble-bridge.overlay, outside the
 * RAM the bootloader zeroes on every reset and outside the kernel's own RAM.
 */
#include "boot_guard.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_power.h>
#include <hal/nrf_wdt.h>

#ifdef __ZEPHYR__
#include <zephyr/devicetree.h>
#if !DT_NODE_HAS_STATUS(DT_NODELABEL(retained0), okay)
#error "The boot guard needs the retained0 region: build with ble-bridge.overlay"
#endif
#endif

#ifndef BOOT_GUARD_HALT
/* Interrupts are already locked here; the watchdog ends the wait. */
#define BOOT_GUARD_HALT() for (;;) { }
#endif

LOG_MODULE_REGISTER(boot_guard, LOG_LEVEL_INF);

#define GUARD_MAGIC 0x50754F46u /* "PuOF" */
/* Value the Adafruit UF2 bootloader checks to stay on the drive. */
#define UF2_BOOTLOADER_MAGIC 0x57u
#define WDT_HZ 32768u
#define FEED_PERIOD_MS (CONFIG_PUCKOFF_BOOT_GUARD_TIMEOUT_S * 1000 / 4)

BUILD_ASSERT(CONFIG_PUCKOFF_BOOT_GUARD_TIMEOUT_S >= 2, "a shorter watchdog cannot cover boot");

struct retained {
	uint32_t magic;
	uint8_t stage;
	uint8_t detail;
	uint8_t watchdog_resets;
	uint8_t fault;
	uint32_t fault_code;
	uint32_t fault_pc;
	char fault_file[BOOT_FAULT_FILE_LEN];
};

static volatile struct retained guard Z_GENERIC_SECTION(RETAINED);
static struct boot_guard_status status;
static atomic_t feeding;
static bool feed_scheduled;

static void feed_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed_work_handler);

void boot_guard_decide(bool watchdog_reset, uint8_t previous_stage, uint8_t watchdog_resets,
		       struct boot_guard_decision *out)
{
	out->safe_mode = false;
	out->bootloader = false;
	if (!watchdog_reset) {
		return;
	}
	switch (previous_stage) {
	case BOOT_STAGE_UNKNOWN:
	case BOOT_STAGE_KERNEL:
	case BOOT_STAGE_BOOTLOADER:
		/* Hung before main(), or interrupted on the way to the drive. */
		out->bootloader = true;
		return;
	case BOOT_STAGE_READY:
		out->safe_mode = watchdog_resets >= CONFIG_PUCKOFF_BOOT_GUARD_MAX_RUNTIME_RESETS;
		return;
	default:
		out->safe_mode = true;
		return;
	}
}

const char *boot_stage_name(uint8_t stage)
{
	static const char *const names[] = {
		"unknown", "kernel", "main", "usb", "bluetooth", "radio", "ready", "bootloader",
	};

	return stage < ARRAY_SIZE(names) ? names[stage] : "invalid";
}

const char *boot_fault_name(uint8_t fault)
{
	static const char *const names[] = { "none", "kernel", "mpsl", "controller" };

	return fault < ARRAY_SIZE(names) ? names[fault] : "invalid";
}

static void feed_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!atomic_get(&feeding)) {
		return;
	}
	nrf_wdt_reload_request_set(NRF_WDT, NRF_WDT_RR0);
	k_work_reschedule(&feed_work, K_MSEC(FEED_PERIOD_MS));
}

static void clear_fault(void)
{
	guard.fault = BOOT_FAULT_NONE;
	guard.fault_code = 0;
	guard.fault_pc = 0;
	for (int i = 0; i < BOOT_FAULT_FILE_LEN; i++) {
		guard.fault_file[i] = 0;
	}
}

static int boot_guard_early(void)
{
	uint32_t cause = nrf_power_resetreas_get(NRF_POWER);
	bool watchdog = (cause & NRF_POWER_RESETREAS_DOG_MASK) != 0;
	uint8_t previous = BOOT_STAGE_UNKNOWN;
	uint8_t resets = 0;
	struct boot_guard_decision decision;

	/* The bootloader leaves RESETREAS alone; it is this application's to clear. */
	nrf_power_resetreas_clear(NRF_POWER, 0xFFFFFFFFu);
	memset(&status, 0, sizeof(status));
	if (guard.magic == GUARD_MAGIC) {
		previous = guard.stage;
		resets = guard.watchdog_resets;
		status.previous_detail = guard.detail;
		status.fault = guard.fault;
		status.fault_code = guard.fault_code;
		status.fault_pc = guard.fault_pc;
		for (int i = 0; i < BOOT_FAULT_FILE_LEN; i++) {
			status.fault_file[i] = guard.fault_file[i];
		}
		status.fault_file[BOOT_FAULT_FILE_LEN - 1] = 0;
	}
	if (watchdog) {
		resets = resets < UINT8_MAX ? resets + 1 : UINT8_MAX;
	} else {
		resets = 0;
	}
	boot_guard_decide(watchdog, previous, resets, &decision);

	status.reset_cause = cause;
	status.previous_stage = previous;
	status.watchdog_resets = resets;
	status.safe_mode = decision.safe_mode;
	status.timeout_s = CONFIG_PUCKOFF_BOOT_GUARD_TIMEOUT_S;

	guard.magic = GUARD_MAGIC;
	guard.watchdog_resets = resets;
	guard.detail = BOOT_DETAIL_NONE;
	clear_fault();
	if (decision.bootloader) {
		guard.stage = BOOT_STAGE_BOOTLOADER;
		nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
		sys_reboot(SYS_REBOOT_COLD);
		return 0;
	}
	guard.stage = BOOT_STAGE_KERNEL;

	if (nrf_wdt_started_check(NRF_WDT)) {
		/* Inherited from the previous run across a soft reset; it cannot be reconfigured. */
		status.watchdog_running = true;
	} else {
		nrf_wdt_behaviour_set(NRF_WDT, NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK);
		nrf_wdt_reload_value_set(NRF_WDT, CONFIG_PUCKOFF_BOOT_GUARD_TIMEOUT_S * WDT_HZ - 1);
		nrf_wdt_reload_request_enable(NRF_WDT, NRF_WDT_RR0);
		nrf_wdt_task_trigger(NRF_WDT, NRF_WDT_TASK_START);
		status.watchdog_running = true;
	}
	atomic_set(&feeding, 1);
	return 0;
}
SYS_INIT(boot_guard_early, PRE_KERNEL_1, 0);

void boot_guard_stage(enum boot_stage stage)
{
	guard.stage = stage;
	guard.detail = BOOT_DETAIL_NONE;
	if (!feed_scheduled && status.watchdog_running) {
		/* The system work queue exists once main() runs; a wedged queue is a hang. */
		feed_scheduled = true;
		k_work_schedule(&feed_work, K_NO_WAIT);
	}
	if (status.safe_mode && stage == BOOT_STAGE_USB) {
		LOG_ERR("safe mode: the previous boot hung at stage %s/%u (%s fault) after %u "
			"watchdog reset(s); Bluetooth and the controller radio stay off",
			boot_stage_name(status.previous_stage), status.previous_detail,
			boot_fault_name(status.fault), status.watchdog_resets);
	}
}

void boot_guard_detail(uint8_t detail)
{
	guard.detail = detail;
}

bool boot_guard_safe_mode(void)
{
	return status.safe_mode;
}

void boot_guard_get_status(struct boot_guard_status *out)
{
	*out = status;
}

int boot_guard_request_bootloader(void)
{
	guard.stage = BOOT_STAGE_BOOTLOADER;
	nrf_power_gpregret_set(NRF_POWER, 0, UF2_BOOTLOADER_MAGIC);
	if (!status.watchdog_running) {
		sys_reboot(SYS_REBOOT_COLD);
		return 0;
	}
	atomic_set(&feeding, 0);
	(void)k_work_cancel_delayable(&feed_work);
	return 0;
}

static void record_fault(uint8_t fault, uint32_t code, uint32_t pc, const char *file)
{
	size_t len = 0;

	if (guard.magic != GUARD_MAGIC) {
		return;
	}
	guard.fault = fault;
	guard.fault_code = code;
	guard.fault_pc = pc;
	if (file) {
		/* Keep the end of the path: the file name, not the directories. */
		len = strlen(file);
		if (len > BOOT_FAULT_FILE_LEN - 1) {
			file += len - (BOOT_FAULT_FILE_LEN - 1);
			len = BOOT_FAULT_FILE_LEN - 1;
		}
	}
	for (size_t i = 0; i < BOOT_FAULT_FILE_LEN; i++) {
		guard.fault_file[i] = i < len ? file[i] : 0;
	}
}

/* Zephyr's fatal error hook: record what happened and let the watchdog reset. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	record_fault(BOOT_FAULT_KERNEL, reason, esf ? esf->basic.pc : 0, NULL);
	BOOT_GUARD_HALT();
}

#ifdef CONFIG_MPSL_ASSERT_HANDLER
void mpsl_assert_handle(char *file, uint32_t line)
{
	record_fault(BOOT_FAULT_MPSL, line, 0, file);
	BOOT_GUARD_HALT();
}
#endif

#ifdef CONFIG_BT_CTLR_ASSERT_HANDLER
void bt_ctlr_assert_handle(char *file, uint32_t line)
{
	record_fault(BOOT_FAULT_CTLR, line, 0, file);
	BOOT_GUARD_HALT();
}
#endif
