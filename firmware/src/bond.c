/* SPDX-License-Identifier: Apache-2.0 */
#include "bond.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

static struct bond bonds[BOND_SLOTS];
static K_MUTEX_DEFINE(bond_lock);

static void encode(const struct bond *b, uint8_t rec[BOND_RECORD_LEN])
{
	memset(rec, 0, BOND_RECORD_LEN);
	sys_put_le32(b->receiver_id, rec);
	sys_put_le32(b->controller_id, rec + 4);
	memcpy(rec + 8, b->serial, strnlen(b->serial, BOND_SERIAL_LEN));
}

static void decode(const uint8_t rec[BOND_RECORD_LEN], struct bond *b)
{
	memset(b, 0, sizeof(*b));
	b->receiver_id = sys_get_le32(rec);
	b->controller_id = sys_get_le32(rec + 4);
	memcpy(b->serial, rec + 8, BOND_SERIAL_LEN);
	b->used = b->receiver_id != 0 || b->controller_id != 0;
}

static int bond_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			     void *cb_arg)
{
	uint8_t rec[BOND_RECORD_LEN];
	int slot;

	if (sscanf(name, "%d", &slot) != 1 || slot < 0 || slot >= BOND_SLOTS ||
	    len != BOND_RECORD_LEN) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, rec, sizeof(rec)) != sizeof(rec)) {
		return -EIO;
	}
	decode(rec, &bonds[slot]);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(bond, "bond", NULL, bond_settings_set, NULL, NULL);

int bond_init(void)
{
	int err = settings_subsys_init();

	if (err) {
		return err;
	}
	return settings_load_subtree("bond");
}

const struct bond *bond_get(int slot)
{
	if (slot < 0 || slot >= BOND_SLOTS) {
		return NULL;
	}
	return &bonds[slot];
}

static int save(int slot)
{
	char key[12];
	uint8_t rec[BOND_RECORD_LEN];

	snprintk(key, sizeof(key), "bond/%d", slot);
	encode(&bonds[slot], rec);
	return settings_save_one(key, rec, sizeof(rec));
}

int bond_set(int slot, uint32_t receiver_id, uint32_t controller_id, const char *serial)
{
	int err;

	if (slot < 0 || slot >= BOND_SLOTS || (receiver_id == 0 && controller_id == 0)) {
		return -EINVAL;
	}
	k_mutex_lock(&bond_lock, K_FOREVER);
	memset(&bonds[slot], 0, sizeof(bonds[slot]));
	bonds[slot].used = true;
	bonds[slot].receiver_id = receiver_id;
	bonds[slot].controller_id = controller_id;
	strncpy(bonds[slot].serial, serial ? serial : "", BOND_SERIAL_LEN);
	err = save(slot);
	k_mutex_unlock(&bond_lock);
	return err;
}

int bond_clear(int slot)
{
	int err;

	if (slot < 0 || slot >= BOND_SLOTS) {
		return -EINVAL;
	}
	k_mutex_lock(&bond_lock, K_FOREVER);
	memset(&bonds[slot], 0, sizeof(bonds[slot]));
	err = save(slot);
	k_mutex_unlock(&bond_lock);
	return err;
}
