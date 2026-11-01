/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include <string.h>
#define CONFIG_PUCKOFF_BOOT_GUARD 1
#define CONFIG_PUCKOFF_BOOT_GUARD_TIMEOUT_S 10
#define CONFIG_PUCKOFF_BOOT_GUARD_MAX_RUNTIME_RESETS 3
#include "../../firmware/src/boot_guard.c"

NRF_POWER_Type test_power;
NRF_WDT_Type test_wdt;
static int reboots;

void sys_reboot(int type)
{
	CHECK(type == SYS_REBOOT_COLD);
	reboots++;
}

/* A fresh power-on: peripherals reset, retained RAM holds garbage. */
static void power_on(uint32_t reset_cause)
{
	memset(&test_power, 0, sizeof(test_power));
	memset(&test_wdt, 0, sizeof(test_wdt));
	memset(&status, 0, sizeof(status));
	memset(&feed_work, 0, sizeof(feed_work));
	feed_work.handler = feed_work_handler;
	feed_scheduled = false;
	atomic_set(&feeding, 0);
	reboots = 0;
	test_power.RESETREAS = reset_cause;
	guard.magic = 0xDEADBEEF;
	guard.stage = 0xEE;
	guard.watchdog_resets = 0xEE;
}

/* A reset that keeps RAM: the record from the previous run survives. */
static void reset_with(uint32_t reset_cause, uint8_t previous_stage, uint8_t previous_resets)
{
	power_on(reset_cause);
	guard.magic = GUARD_MAGIC;
	guard.stage = previous_stage;
	guard.watchdog_resets = previous_resets;
}

static void check_decision(bool watchdog, uint8_t stage, uint8_t resets, bool safe, bool loader)
{
	struct boot_guard_decision d;

	boot_guard_decide(watchdog, stage, resets, &d);
	CHECK(d.safe_mode == safe && d.bootloader == loader);
}

int main(void)
{
	/* Policy table. */
	for (uint8_t stage = 0; stage <= BOOT_STAGE_BOOTLOADER; stage++) {
		check_decision(false, stage, 0, false, false);
		check_decision(false, stage, 200, false, false);
	}
	check_decision(true, BOOT_STAGE_UNKNOWN, 1, false, true);
	check_decision(true, BOOT_STAGE_KERNEL, 1, false, true);
	check_decision(true, BOOT_STAGE_BOOTLOADER, 1, false, true);
	check_decision(true, BOOT_STAGE_MAIN, 1, true, false);
	check_decision(true, BOOT_STAGE_USB, 1, true, false);
	check_decision(true, BOOT_STAGE_BLUETOOTH, 1, true, false);
	check_decision(true, BOOT_STAGE_RADIO, 1, true, false);
	check_decision(true, BOOT_STAGE_READY, 1, false, false);
	check_decision(true, BOOT_STAGE_READY, 2, false, false);
	check_decision(true, BOOT_STAGE_READY, 3, true, false);
	check_decision(true, BOOT_STAGE_READY, 255, true, false);
	CHECK(strcmp(boot_stage_name(BOOT_STAGE_READY), "ready") == 0);
	CHECK(strcmp(boot_stage_name(BOOT_STAGE_BOOTLOADER), "bootloader") == 0);
	CHECK(strcmp(boot_stage_name(42), "invalid") == 0);

	/* Cold boot: watchdog armed, record initialized, reset reason consumed. */
	power_on(0);
	CHECK(boot_guard_early() == 0);
	CHECK(test_power.RESETREAS == 0);
	CHECK(test_wdt.starts == 1 && test_wdt.RUNSTATUS == 1);
	CHECK(test_wdt.CONFIG == NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK);
	CHECK(test_wdt.CRV == 10u * 32768u - 1u);
	CHECK(test_wdt.RREN == 1u);
	CHECK(guard.magic == GUARD_MAGIC && guard.stage == BOOT_STAGE_KERNEL);
	CHECK(guard.watchdog_resets == 0);
	CHECK(!boot_guard_safe_mode() && reboots == 0);
	struct boot_guard_status st;

	boot_guard_get_status(&st);
	CHECK(st.previous_stage == BOOT_STAGE_UNKNOWN && st.watchdog_resets == 0);
	CHECK(st.watchdog_running && st.timeout_s == 10 && st.reset_cause == 0);

	/* The feed starts with the first stage from main() and keeps itself going. */
	CHECK(!feed_work.scheduled);
	boot_guard_stage(BOOT_STAGE_MAIN);
	CHECK(guard.stage == BOOT_STAGE_MAIN && feed_work.scheduled && feed_work.delay == 0);
	feed_work.handler(NULL);
	CHECK(test_wdt.RR[0] == 1 && feed_work.scheduled && feed_work.delay == 2500);
	boot_guard_stage(BOOT_STAGE_USB);
	boot_guard_stage(BOOT_STAGE_READY);
	CHECK(guard.stage == BOOT_STAGE_READY);
	feed_work.handler(NULL);
	CHECK(test_wdt.RR[0] == 2);

	/* Leaving for the UF2 drive: magic set, feeding stops, the watchdog resets us. */
	CHECK(boot_guard_request_bootloader() == 0);
	CHECK(test_power.GPREGRET[0] == 0x57 && guard.stage == BOOT_STAGE_BOOTLOADER);
	CHECK(!feed_work.scheduled && reboots == 0);
	feed_work.scheduled = true;
	feed_work.handler(NULL);
	CHECK(test_wdt.RR[0] == 2);

	/* A hang after main() comes back in safe mode, with the watchdog still armed. */
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_BLUETOOTH, 0);
	CHECK(boot_guard_early() == 0);
	CHECK(boot_guard_safe_mode() && reboots == 0 && test_wdt.starts == 1);
	boot_guard_get_status(&st);
	CHECK(st.previous_stage == BOOT_STAGE_BLUETOOTH && st.watchdog_resets == 1);
	CHECK(st.reset_cause == NRF_POWER_RESETREAS_DOG_MASK);
	CHECK(guard.watchdog_resets == 1 && guard.stage == BOOT_STAGE_KERNEL);

	/* A clean reset afterwards clears the streak and leaves safe mode. */
	reset_with(NRF_POWER_RESETREAS_RESETPIN_MASK, BOOT_STAGE_USB, 1);
	CHECK(boot_guard_early() == 0);
	CHECK(!boot_guard_safe_mode() && guard.watchdog_resets == 0);

	/* A ready receiver restarts normally until the streak limit. */
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_READY, 0);
	CHECK(boot_guard_early() == 0 && !boot_guard_safe_mode() && guard.watchdog_resets == 1);
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_READY, 2);
	CHECK(boot_guard_early() == 0 && boot_guard_safe_mode() && guard.watchdog_resets == 3);
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_READY, 255);
	CHECK(boot_guard_early() == 0 && boot_guard_safe_mode() && guard.watchdog_resets == 255);

	/* A hang before main() goes to the drive without arming the watchdog. */
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_KERNEL, 0);
	CHECK(boot_guard_early() == 0);
	CHECK(reboots == 1 && test_power.GPREGRET[0] == 0x57 && test_wdt.starts == 0);
	CHECK(guard.stage == BOOT_STAGE_BOOTLOADER && !boot_guard_safe_mode());

	/* So does a watchdog that bit while the drive was already the destination. */
	reset_with(NRF_POWER_RESETREAS_DOG_MASK, BOOT_STAGE_BOOTLOADER, 1);
	CHECK(boot_guard_early() == 0 && reboots == 1 && test_wdt.starts == 0);
	CHECK(test_power.GPREGRET[0] == 0x57);

	/* A watchdog reset with no valid record means nothing of ours ever ran. */
	power_on(NRF_POWER_RESETREAS_DOG_MASK);
	CHECK(boot_guard_early() == 0 && reboots == 1 && test_wdt.starts == 0);

	/* After the drive reflashes us, the bootloader's soft reset is a normal boot. */
	reset_with(NRF_POWER_RESETREAS_SREQ_MASK, BOOT_STAGE_BOOTLOADER, 0);
	CHECK(boot_guard_early() == 0 && reboots == 0 && test_wdt.starts == 1);
	CHECK(!boot_guard_safe_mode());

	/* A watchdog inherited across a soft reset is adopted, not reconfigured. */
	reset_with(NRF_POWER_RESETREAS_SREQ_MASK, BOOT_STAGE_READY, 0);
	test_wdt.RUNSTATUS = 1;
	test_wdt.CRV = 12345;
	CHECK(boot_guard_early() == 0 && test_wdt.starts == 0 && test_wdt.CRV == 12345);
	boot_guard_get_status(&st);
	CHECK(st.watchdog_running);

	/* Without a running watchdog the bootloader request reboots directly. */
	status.watchdog_running = false;
	CHECK(boot_guard_request_bootloader() == 0 && reboots == 1);
	CHECK(test_power.GPREGRET[0] == 0x57);

	puts("boot_guard: watchdog arming, stage policy, safe mode and UF2 fallback");
	return 0;
}
