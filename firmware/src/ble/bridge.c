/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "bridge.h"
#include "gamepad.h"
#include "remote.h"
#include "../boot_guard.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(ble_bridge, LOG_LEVEL_INF);

#define MODES_KEY "puckoff/ble_modes"

static K_MUTEX_DEFINE(bridge_lock);
static struct ble_mode_table table;
static bool have_host;
static uint8_t host_peer[BLE_PEER_LEN];
static uint8_t active_mode;
static uint8_t active_profile;
static struct remote_state remote;
static struct remote_chord chord;

static int modes_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	uint8_t buf[BLE_MODE_TABLE_BYTES];
	const char *next;
	ssize_t got;

	if (settings_name_steq(name, "ble_modes", &next) && !next) {
		if (len != sizeof(buf)) {
			return -EINVAL;
		}
		got = read_cb(cb_arg, buf, sizeof(buf));
		if (got != (ssize_t)sizeof(buf)) {
			return -EIO;
		}
		if (!ble_modes_decode(&table, buf, sizeof(buf))) {
			LOG_WRN("stored host modes invalid; ignoring them");
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(puckoff, "puckoff", NULL, modes_set, NULL, NULL);

static int save_locked(void)
{
	uint8_t buf[BLE_MODE_TABLE_BYTES];

	ble_modes_encode(&table, buf);
	return settings_save_one(MODES_KEY, buf, sizeof(buf));
}

const char *ble_bridge_mode_name(uint8_t mode)
{
	return mode == BLE_MODE_REMOTE ? "remote" : "gamepad";
}

const char *ble_bridge_profile_name(uint8_t profile)
{
	return profile == REMOTE_PROFILE_APPLE ? "apple" : "android";
}

int ble_bridge_start(void)
{
	int err = ble_gamepad_start();

	if (err) {
		return err;
	}
	err = settings_load_subtree("puckoff");
	if (err) {
		LOG_WRN("host modes not loaded (%d); defaults apply", err);
	}
	boot_guard_detail(7); /* bootguard detail 7: host modes loaded */
	return 0;
}

/* Called with the lock held; true when the connected host changed. */
static bool track_host_locked(void)
{
	uint8_t peer[BLE_PEER_LEN];

	if (!ble_gamepad_host_peer(peer)) {
		have_host = false;
		return false;
	}
	if (have_host && memcmp(peer, host_peer, sizeof(peer)) == 0) {
		return false;
	}
	have_host = true;
	memcpy(host_peer, peer, sizeof(peer));
	ble_modes_lookup(&table, peer, &active_mode, &active_profile);
	remote_reset(&remote);
	memset(&chord, 0, sizeof(chord));
	LOG_INF("BLE host: %s mode, %s profile", ble_bridge_mode_name(active_mode),
		ble_bridge_profile_name(active_profile));
	return true;
}

static void switch_mode_locked(uint8_t mode)
{
	int err;

	active_mode = mode;
	remote_reset(&remote);
	ble_gamepad_neutral();
	if (ble_modes_store(&table, host_peer, active_mode, active_profile)) {
		err = save_locked();
		if (err) {
			LOG_WRN("host mode not saved (%d)", err);
		}
	}
	LOG_INF("BLE mode: %s", ble_bridge_mode_name(active_mode));
}

bool ble_bridge_input(const struct ctrl_state *in, const struct gamepad_report *report)
{
	struct ctrl_state masked;

	k_mutex_lock(&bridge_lock, K_FOREVER);
	track_host_locked();
	if (!have_host) {
		k_mutex_unlock(&bridge_lock);
		return false;
	}
	if (remote_chord_step(&chord, in->buttons, k_uptime_get_32())) {
		switch_mode_locked(active_mode == BLE_MODE_REMOTE ? BLE_MODE_GAMEPAD :
								     BLE_MODE_REMOTE);
	}
	if (chord.held) {
		/* The chord itself must not reach the host as Home plus Menu. */
		masked = *in;
		masked.buttons &= ~REMOTE_CHORD_BUTTONS;
		in = &masked;
	}
	if (active_mode == BLE_MODE_REMOTE) {
		struct remote_report out;

		remote_from_ctrl(&remote, in, active_profile, &out);
		(void)ble_gamepad_update_remote(&out);
	} else if (chord.held) {
		struct gamepad_report quiet;

		gamepad_from_ctrl(in, &quiet);
		(void)ble_gamepad_update(&quiet);
	} else {
		(void)ble_gamepad_update(report);
	}
	k_mutex_unlock(&bridge_lock);
	return true;
}

bool ble_bridge_lost(void)
{
	bool taken;

	k_mutex_lock(&bridge_lock, K_FOREVER);
	track_host_locked();
	taken = have_host;
	if (taken) {
		remote_reset(&remote);
		memset(&chord, 0, sizeof(chord));
		ble_gamepad_neutral();
	}
	k_mutex_unlock(&bridge_lock);
	return taken;
}

int ble_bridge_set_mode(uint8_t mode)
{
	if (mode >= BLE_MODE_COUNT) {
		return -EINVAL;
	}
	k_mutex_lock(&bridge_lock, K_FOREVER);
	track_host_locked();
	if (!have_host) {
		k_mutex_unlock(&bridge_lock);
		return -ENOTCONN;
	}
	switch_mode_locked(mode);
	k_mutex_unlock(&bridge_lock);
	return 0;
}

int ble_bridge_set_profile(uint8_t profile)
{
	if (profile >= REMOTE_PROFILE_COUNT) {
		return -EINVAL;
	}
	k_mutex_lock(&bridge_lock, K_FOREVER);
	track_host_locked();
	if (!have_host) {
		k_mutex_unlock(&bridge_lock);
		return -ENOTCONN;
	}
	active_profile = profile;
	switch_mode_locked(active_mode);
	k_mutex_unlock(&bridge_lock);
	return 0;
}

int ble_bridge_forget_hosts(void)
{
	int err;

	k_mutex_lock(&bridge_lock, K_FOREVER);
	err = ble_gamepad_forget_host();
	if (!err) {
		ble_modes_clear(&table);
		have_host = false;
		err = save_locked();
	}
	k_mutex_unlock(&bridge_lock);
	return err;
}

void ble_bridge_get_status(struct ble_bridge_status *status)
{
	k_mutex_lock(&bridge_lock, K_FOREVER);
	track_host_locked();
	status->connected = have_host;
	status->mode = have_host ? active_mode : BLE_MODE_GAMEPAD;
	status->profile = have_host ? active_profile : REMOTE_PROFILE_ANDROID;
	status->remembered_hosts = 0;
	for (int i = 0; i < BLE_MODE_HOSTS; i++) {
		static const uint8_t none[BLE_PEER_LEN];

		if (memcmp(table.host[i].peer, none, BLE_PEER_LEN) != 0) {
			status->remembered_hosts++;
		}
	}
	k_mutex_unlock(&bridge_lock);
}
