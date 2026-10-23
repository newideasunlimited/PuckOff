/* SPDX-License-Identifier: Apache-2.0 */
#include "bond.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

static struct bond bonds[BOND_SLOTS];
static K_MUTEX_DEFINE(bond_lock);
static struct k_spinlock bond_data_lock;
static bool storage_ready;
static struct bond loaded_bonds[BOND_SLOTS];
static bool loading;
static int load_error;

static void publish(int slot, const struct bond *b)
{
	k_spinlock_key_t key = k_spin_lock(&bond_data_lock);

	bonds[slot] = *b;
	k_spin_unlock(&bond_data_lock, key);
}

static void encode(const struct bond *b, uint8_t rec[BOND_RECORD_LEN])
{
	memset(rec, 0, BOND_RECORD_LEN);
	sys_put_le32(b->receiver_id, rec);
	sys_put_le32(b->controller_id, rec + 4);
	memcpy(rec + 8, b->serial, strnlen(b->serial, BOND_SERIAL_LEN));
}

static int decode(const uint8_t rec[BOND_RECORD_LEN], struct bond *b)
{
	bool padding = false;

	memset(b, 0, sizeof(*b));
	b->receiver_id = sys_get_le32(rec);
	b->controller_id = sys_get_le32(rec + 4);
	b->used = b->receiver_id != 0 || b->controller_id != 0;
	if (b->used && (b->receiver_id == 0 || b->controller_id == 0)) {
		return -EINVAL;
	}
	for (int i = 0; i < BOND_SERIAL_LEN; i++) {
		uint8_t c = rec[8 + i];

		if (c == 0) {
			padding = true;
		} else if (!b->used || padding || c < 0x21 || c > 0x7e) {
			return -EINVAL;
		}
		b->serial[i] = (char)c;
	}
	return 0;
}

static bool same_bond(const struct bond *a, const struct bond *b)
{
	return a->used == b->used && a->receiver_id == b->receiver_id &&
	       a->controller_id == b->controller_id && strcmp(a->serial, b->serial) == 0;
}

static int bond_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			     void *cb_arg)
{
	uint8_t rec[BOND_RECORD_LEN];
	int slot, err = 0;

	if (!loading) {
		return -EACCES;
	}
	if (name == NULL || name[0] < '0' || name[0] >= '0' + BOND_SLOTS ||
	    name[1] != '\0' || (len != BOND_RECORD_LEN && len != 0)) {
		err = -EINVAL;
		goto fail;
	}
	slot = name[0] - '0';
	if (len == 0) {
		memset(&loaded_bonds[slot], 0, sizeof(loaded_bonds[slot]));
		return 0;
	}
	if (read_cb(cb_arg, rec, sizeof(rec)) != (ssize_t)sizeof(rec)) {
		err = -EIO;
		goto fail;
	}
	err = decode(rec, &loaded_bonds[slot]);
	if (err) {
		goto fail;
	}
	return 0;

fail:
	/* Some settings backends log handler errors without returning them to load. */
	if (!load_error) {
		load_error = err;
	}
	return err;
}

SETTINGS_STATIC_HANDLER_DEFINE(bond, "bond", NULL, bond_settings_set, NULL, NULL);

int bond_init(void)
{
	int err;

	k_mutex_lock(&bond_lock, K_FOREVER);
	if (bond_storage_ready()) {
		k_mutex_unlock(&bond_lock);
		return 0;
	}
	memset(loaded_bonds, 0, sizeof(loaded_bonds));
	load_error = 0;
	loading = true;
	err = settings_subsys_init();
	if (!err) {
		err = settings_load_subtree("bond");
	}
	loading = false;
	err = err ? err : load_error;
	if (!err) {
		k_spinlock_key_t key = k_spin_lock(&bond_data_lock);

		memcpy(bonds, loaded_bonds, sizeof(bonds));
		storage_ready = true;
		k_spin_unlock(&bond_data_lock, key);
	}
	k_mutex_unlock(&bond_lock);
	return err;
}

bool bond_storage_ready(void)
{
	k_spinlock_key_t key = k_spin_lock(&bond_data_lock);
	bool ready = storage_ready;

	k_spin_unlock(&bond_data_lock, key);
	return ready;
}

int bond_snapshot(int slot, struct bond *out)
{
	k_spinlock_key_t key;

	if (slot < 0 || slot >= BOND_SLOTS || out == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&bond_data_lock);
	*out = bonds[slot];
	k_spin_unlock(&bond_data_lock, key);
	return 0;
}

int bond_export(uint8_t records[BOND_SLOTS][BOND_RECORD_LEN])
{
	struct bond snapshot[BOND_SLOTS];
	k_spinlock_key_t key;

	if (records == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&bond_data_lock);
	if (!storage_ready) {
		k_spin_unlock(&bond_data_lock, key);
		return -EACCES;
	}
	memcpy(snapshot, bonds, sizeof(snapshot));
	k_spin_unlock(&bond_data_lock, key);
	for (int i = 0; i < BOND_SLOTS; i++) {
		encode(&snapshot[i], records[i]);
	}
	return 0;
}

static int save(int slot, const struct bond *candidate)
{
	char key[12];
	uint8_t rec[BOND_RECORD_LEN];

	snprintk(key, sizeof(key), "bond/%d", slot);
	encode(candidate, rec);
	return settings_save_one(key, rec, sizeof(rec));
}

static int update(int slot, const struct bond *candidate, const struct bond *expected,
		  bool overwrite)
{
	struct bond current;
	int err;

	if (slot < 0 || slot >= BOND_SLOTS) {
		return -EINVAL;
	}
	k_mutex_lock(&bond_lock, K_FOREVER);
	if (!bond_storage_ready()) {
		err = -EACCES;
		goto done;
	}
	bond_snapshot(slot, &current);
	if ((expected != NULL && !same_bond(&current, expected)) ||
	    (!overwrite && current.used && !same_bond(&current, candidate))) {
		err = -EAGAIN;
		goto done;
	}
	if (same_bond(&current, candidate)) {
		err = 0;
		goto done;
	}
	if (candidate->used) {
		for (int i = 0; i < BOND_SLOTS; i++) {
			struct bond other;

			bond_snapshot(i, &other);
			if (i != slot && other.used &&
			    other.controller_id == candidate->controller_id) {
				err = -EAGAIN;
				goto done;
			}
		}
	}
	err = save(slot, candidate);
	if (!err) {
		publish(slot, candidate);
	}

done:
	k_mutex_unlock(&bond_lock);
	return err;
}

int bond_import(int slot, const uint8_t record[BOND_RECORD_LEN],
		const uint8_t expected[BOND_RECORD_LEN])
{
	struct bond candidate, before;

	if (record == NULL || expected == NULL || decode(record, &candidate) ||
	    !candidate.used || decode(expected, &before)) {
		return -EINVAL;
	}
	return update(slot, &candidate, &before, true);
}

int bond_clear_if(int slot, const uint8_t expected[BOND_RECORD_LEN])
{
	const struct bond empty = {0};
	struct bond before;

	if (expected == NULL || decode(expected, &before)) {
		return -EINVAL;
	}
	return update(slot, &empty, &before, true);
}

int bond_set(int slot, uint32_t receiver_id, uint32_t controller_id, const char *serial)
{
	struct bond candidate = {
		.used = true,
		.receiver_id = receiver_id,
		.controller_id = controller_id,
	};
	size_t len = serial == NULL ? 0 : strnlen(serial, BOND_SERIAL_LEN + 1);

	if (receiver_id == 0 || controller_id == 0 || len > BOND_SERIAL_LEN) {
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i++) {
		if ((unsigned char)serial[i] < 0x21 || (unsigned char)serial[i] > 0x7e) {
			return -EINVAL;
		}
		candidate.serial[i] = serial[i];
	}
	return update(slot, &candidate, NULL, false);
}

int bond_clear(int slot)
{
	const struct bond empty = {0};

	return update(slot, &empty, NULL, true);
}
