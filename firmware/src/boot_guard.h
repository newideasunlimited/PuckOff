/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Boot guard: a hardware watchdog armed before the kernel, boot-stage markers
 * in retained RAM, and a small policy that turns a hung boot into a boot that
 * can be inspected (safe mode) or reflashed (the UF2 drive) without a paperclip.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Stages are ordered: a later stage means more of the firmware came up. */
enum boot_stage {
	BOOT_STAGE_UNKNOWN = 0,    /* retained memory held no valid record */
	BOOT_STAGE_KERNEL = 1,     /* guard armed; kernel and drivers initializing */
	BOOT_STAGE_MAIN = 2,       /* main() entered */
	BOOT_STAGE_USB = 3,        /* USB device started */
	BOOT_STAGE_BLUETOOTH = 4,  /* Bluetooth stack starting */
	BOOT_STAGE_RADIO = 5,      /* controller radio starting */
	BOOT_STAGE_READY = 6,      /* every transport running */
	BOOT_STAGE_BOOTLOADER = 7, /* deliberately on the way to the UF2 drive */
};

struct boot_guard_decision {
	bool safe_mode;  /* start USB and the console only */
	bool bootloader; /* go straight to the UF2 drive, without a watchdog */
};

struct boot_guard_status {
	uint32_t reset_cause;    /* raw RESETREAS captured at this boot */
	uint8_t previous_stage;  /* enum boot_stage the previous run reached */
	uint8_t watchdog_resets; /* consecutive watchdog resets, this one included */
	bool safe_mode;
	bool watchdog_running;
	uint16_t timeout_s;
};

/*
 * Pure policy, shared with the host tests. A boot after a watchdog reset is
 * normal when the previous run was fully up (until too many in a row), safe
 * mode when the previous run hung after main() started, and the bootloader
 * when it hung before main() or while already heading for the UF2 drive.
 */
void boot_guard_decide(bool watchdog_reset, uint8_t previous_stage, uint8_t watchdog_resets,
		       struct boot_guard_decision *out);
const char *boot_stage_name(uint8_t stage);

#ifdef CONFIG_PUCKOFF_BOOT_GUARD
void boot_guard_stage(enum boot_stage stage);
bool boot_guard_safe_mode(void);
void boot_guard_get_status(struct boot_guard_status *out);
/*
 * Arm a reboot into the UF2 drive. With the watchdog running, this stops
 * feeding it and returns 0: the caller must idle until the watchdog resets
 * the chip (at most the configured timeout). Without a watchdog it reboots
 * immediately and does not return.
 */
int boot_guard_request_bootloader(void);
#else
static inline void boot_guard_stage(enum boot_stage stage)
{
	(void)stage;
}
static inline bool boot_guard_safe_mode(void)
{
	return false;
}
#endif
