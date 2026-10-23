/* SPDX-License-Identifier: Apache-2.0 */
/*
 * PuckOff: open receiver firmware for the Steam Controller.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "bond.h"
#include "gamepad.h"
#include "input.h"
#include "link.h"
#include "usb.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static int active_slot = -1;
static bool watch;
static struct ctrl_state last_seen;

void main_set_watch(bool on)
{
	watch = on;
}

static void on_input(int slot, const struct ctrl_state *st)
{
	struct gamepad_report r;

	/* One gamepad per receiver for now: the most recently active controller drives it. */
	if (active_slot != slot) {
		active_slot = slot;
	}
	gamepad_from_ctrl(st, &r);
	usb_send_gamepad(&r);

	if (watch && (st->buttons != last_seen.buttons ||
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
	}
}

int main(void)
{
	int err;

	if (gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	err = bond_init();
	if (err) {
		LOG_ERR("pairing storage failed (%d)", err);
	}
	err = usb_start();
	if (err) {
		LOG_ERR("usb failed (%d)", err);
	}
	link_start(on_input, on_lost);
	LOG_INF("PuckOff ready");

	/* LED: slow blink = searching, solid = connected, fast blink = not paired. */
	for (int t = 0;; t++) {
		bool paired = false;

		for (int i = 0; i < BOND_SLOTS; i++) {
			paired |= bond_get(i)->used;
		}
		if (link_any_connected()) {
			gpio_pin_set_dt(&led, 1);
		} else if (paired) {
			gpio_pin_set_dt(&led, (t % 10) == 0);
		} else {
			gpio_pin_set_dt(&led, t & 1);
		}
		k_msleep(100);
	}
	return 0;
}
