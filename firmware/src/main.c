/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PuckOff: open receiver firmware for the Steam Controller.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "bond.h"
#include "gamepad.h"
#include "input.h"
#include "link.h"
#include "usb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static int active_slot = -1;
static atomic_t watch;
static struct ctrl_state last_seen;

void main_set_watch(bool on)
{
	atomic_set(&watch, on);
}

static void on_input(int slot, const struct ctrl_state *st)
{
	struct gamepad_report r;

	/* Keep the first input owner until it is explicitly released by the link. */
	if (active_slot >= 0 && active_slot != slot) {
		return;
	}
	active_slot = slot;
	gamepad_from_ctrl(st, &r);
	usb_send_gamepad(&r);

	if (atomic_get(&watch) && (st->buttons != last_seen.buttons ||
		      abs(st->lx - last_seen.lx) > 2000 || abs(st->ly - last_seen.ly) > 2000 ||
		      abs(st->rx - last_seen.rx) > 2000 || abs(st->ry - last_seen.ry) > 2000 ||
		      abs(st->lt - last_seen.lt) > 2000 || abs(st->rt - last_seen.rt) > 2000)) {
		last_seen = *st;
		printk("in%d btn=%08X L=(%6d,%6d) R=(%6d,%6d) LT=%5u RT=%5u\n", slot, st->buttons,
		       st->lx, st->ly, st->rx, st->ry, st->lt, st->rt);
	}
}

static void on_lost(int slot)
{
	struct gamepad_report r;

	if (slot == active_slot) {
		gamepad_neutral(&r);
		usb_send_gamepad(&r);
		active_slot = -1;
		last_seen = (struct ctrl_state){0};
	}
}

int main(void)
{
	int err;
	bool have_led = gpio_is_ready_dt(&led);

	if (have_led) {
		err = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
		if (err) {
			LOG_WRN("LED setup failed (%d)", err);
			have_led = false;
		}
	}

	int storage_err = bond_init();

	if (storage_err) {
		LOG_ERR("pairing storage failed (%d); radio will not start", storage_err);
	}
	err = usb_start();
	if (err) {
		LOG_ERR("usb failed (%d)", err);
		return err;
	}
	if (storage_err) {
		return storage_err;
	}
	err = link_start(on_input, on_lost);
	if (err) {
		LOG_ERR("radio startup failed (%d)", err);
		return err;
	}
	LOG_INF("PuckOff ready");

	/* LED: slow blink = searching, solid = connected, fast blink = not paired. */
	for (uint32_t t = 0;; t++) {
		bool paired = false;

		for (int i = 0; i < BOND_SLOTS; i++) {
			struct bond b;

			err = bond_snapshot(i, &b);
			if (err) {
				LOG_ERR("slot %d: bond snapshot failed (%d)", i, err);
			} else {
				paired |= b.used;
			}
		}
		if (have_led) {
			int value = link_any_connected() ? 1 : paired ? (t % 10) == 0 : t & 1;

			err = gpio_pin_set_dt(&led, value);
			if (err) {
				LOG_WRN("LED update failed (%d)", err);
				have_led = false;
			}
		}
		k_msleep(100);
	}
	return 0;
}
