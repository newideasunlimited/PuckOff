/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>

/* Exercise the actual implementation, including its shell argument parsers. */
#include "../../firmware/src/bond.c"
#include "../../firmware/src/console.c"

struct test_power_registers test_power;
static const struct shell test_shell;
static char output[8192];
static size_t output_len;
static uint8_t flash_records[BOND_SLOTS][BOND_RECORD_LEN];
static bool flash_present[BOND_SLOTS];
static int init_error, backend_load_error, save_error, writes;
static const char *bad_key;
static size_t bad_length;
static ssize_t short_read;
static bool inject_bad_load, watch_enabled, maximum_stats;
static atomic_bool pause_save, save_entered, release_save, writer_done;

static const uint8_t record_a[BOND_RECORD_LEN] = {
	0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
	'S', 'Y', 'N', 'T', 'H', 'E', 'T', 'I', 'C', '-', 'A',
};
static const uint8_t record_b[BOND_RECORD_LEN] = {
	0x3d, 0x2c, 0x1b, 0x0a, 0x73, 0x62, 0x51, 0x40,
	'S', 'Y', 'N', 'T', 'H', 'E', 'T', 'I', 'C', '-', 'B',
};
static const uint8_t empty_record[BOND_RECORD_LEN];

static void reset_output(void)
{
	memset(output, 0, sizeof(output));
	output_len = 0;
}

void test_shell_print(const struct shell *sh, const char *format, ...)
{
	va_list args;
	int len;

	va_start(args, format);
	len = vsnprintf(output + output_len, sizeof(output) - output_len, format, args);
	va_end(args);
	assert(len >= 0 && (size_t)len + 1 < sizeof(output) - output_len);
	output_len += (size_t)len;
	output[output_len++] = '\n';
	output[output_len] = '\0';
}

void link_get_stats(int slot, struct link_stats *out)
{
	*out = (struct link_stats){
		.connected = true,
		.rssi = maximum_stats ? UINT8_MAX : 47,
		.polls = maximum_stats ? UINT32_MAX : 1000,
		.replies = maximum_stats ? UINT32_MAX : 995,
		.reports = maximum_stats ? UINT32_MAX : 990,
		.crc_errors = maximum_stats ? UINT32_MAX : 2,
	};
}

void link_set_verbose(bool on)
{
}

void main_set_watch(bool on)
{
	watch_enabled = on;
}

static ssize_t read_record(void *arg, void *data, size_t len)
{
	if (short_read) {
		return short_read;
	}
	memcpy(data, arg, len);
	return (ssize_t)len;
}

int settings_subsys_init(void)
{
	return init_error;
}

int settings_load_subtree(const char *subtree)
{
	assert(strcmp(subtree, "bond") == 0);
	if (backend_load_error) {
		return backend_load_error;
	}
	for (int i = 0; i < BOND_SLOTS; i++) {
		char key[2] = {(char)('0' + i), '\0'};

		if (flash_present[i]) {
			(void)bond_settings_set(key, BOND_RECORD_LEN, read_record, flash_records[i]);
		}
	}
	if (inject_bad_load) {
		/* Deliberately swallow callback errors, like settings backends may do. */
		(void)bond_settings_set(bad_key, bad_length, read_record, flash_records[0]);
	}
	return 0;
}

int settings_save_one(const char *key, const void *data, size_t len)
{
	int slot;
	struct bond snapshot;

	assert(strlen(key) == 6 && strncmp(key, "bond/", 5) == 0);
	slot = key[5] - '0';
	assert(slot >= 0 && slot < BOND_SLOTS && len == BOND_RECORD_LEN);
	/* This would deadlock if a writer held the snapshot lock during I/O. */
	assert(bond_snapshot(slot, &snapshot) == 0);
	writes++;
	if (atomic_load(&pause_save)) {
		atomic_store(&save_entered, true);
		while (!atomic_load(&release_save)) {
			sched_yield();
		}
	}
	if (save_error) {
		return save_error;
	}
	memcpy(flash_records[slot], data, len);
	flash_present[slot] = true;
	return 0;
}

static void reset_runtime(void)
{
	memset(bonds, 0, sizeof(bonds));
	memset(loaded_bonds, 0, sizeof(loaded_bonds));
	storage_ready = false;
	loading = false;
	load_error = 0;
}

static void reset_all(void)
{
	reset_runtime();
	memset(flash_records, 0, sizeof(flash_records));
	memset(flash_present, 0, sizeof(flash_present));
	init_error = backend_load_error = save_error = writes = 0;
	inject_bad_load = false;
	short_read = 0;
	maximum_stats = false;
	atomic_store(&pause_save, false);
	atomic_store(&save_entered, false);
	atomic_store(&release_save, false);
	atomic_store(&writer_done, false);
	reset_output();
}

static void assert_record(int slot, const uint8_t expected[BOND_RECORD_LEN])
{
	struct bond b;
	uint8_t actual[BOND_RECORD_LEN];

	assert(bond_snapshot(slot, &b) == 0);
	encode(&b, actual);
	assert(memcmp(actual, expected, BOND_RECORD_LEN) == 0);
	assert(b.used == (memcmp(expected, empty_record, BOND_RECORD_LEN) != 0));
}

static void test_storage(void)
{
	uint8_t exported[BOND_SLOTS][BOND_RECORD_LEN];
	struct bond snapshot;

	reset_all();
	assert(!bond_storage_ready());
	assert(bond_snapshot(0, &snapshot) == 0 && !snapshot.used);
	assert(bond_set(0, 1, 2, "SYNTHETIC") == -EACCES);
	assert(bond_clear(0) == -EACCES);
	assert(bond_export(exported) == -EACCES);
	init_error = -EIO;
	assert(bond_init() == -EIO && !bond_storage_ready());
	init_error = 0;
	backend_load_error = -EIO;
	assert(bond_init() == -EIO && !bond_storage_ready());
	backend_load_error = 0;
	assert(bond_init() == 0 && bond_storage_ready());
	assert(bond_import(0, record_a, empty_record) == 0);
	assert(memcmp(flash_records[0], record_a, BOND_RECORD_LEN) == 0);
	reset_runtime();
	assert(bond_init() == 0 && bond_storage_ready());
	assert_record(0, record_a);
	assert(bond_export(exported) == 0);
	assert(memcmp(exported[0], record_a, BOND_RECORD_LEN) == 0);
	assert(memcmp(exported[3], empty_record, BOND_RECORD_LEN) == 0);
	assert(bond_clear_if(0, record_a) == 0);
	reset_runtime();
	assert(bond_init() == 0);
	assert_record(0, empty_record);
	assert(bond_snapshot(-1, &snapshot) == -EINVAL);
	assert(bond_snapshot(4, &snapshot) == -EINVAL);
	assert(bond_snapshot(0, NULL) == -EINVAL);
	assert(bond_export(NULL) == -EINVAL);
}

static void test_validation(void)
{
	uint8_t bad[BOND_RECORD_LEN];
	const char *serials[] = {"12345678901234567", "BAD\nSERIAL", "BAD SERIAL", "BAD\xff"};

	reset_all();
	assert(bond_init() == 0);
	assert(bond_set(-1, 1, 2, "") == -EINVAL);
	assert(bond_set(4, 1, 2, "") == -EINVAL);
	assert(bond_set(0, 0, 2, "") == -EINVAL);
	assert(bond_set(0, 1, 0, "") == -EINVAL);
	for (size_t i = 0; i < sizeof(serials) / sizeof(serials[0]); i++) {
		assert(bond_set(0, 1, 2, serials[i]) == -EINVAL);
	}
	assert(bond_import(0, NULL, empty_record) == -EINVAL);
	assert(bond_import(0, record_a, NULL) == -EINVAL);
	assert(bond_import(0, empty_record, empty_record) == -EINVAL);
	assert(bond_clear_if(0, NULL) == -EINVAL);
	for (int kind = 0; kind < 4; kind++) {
		memcpy(bad, record_a, sizeof(bad));
		if (kind == 0) {
			memset(bad, 0, 4);
		} else if (kind == 1) {
			bad[10] = 0;
		} else if (kind == 2) {
			bad[8] = 0x7f;
		} else {
			bad[8] = 0x80;
		}
		assert(bond_import(0, bad, empty_record) == -EINVAL);
		assert(bond_import(0, record_a, bad) == -EINVAL);
	}
	assert(writes == 0);
	assert(bond_set(0, 1, 2, "ABCDEFGHIJKLMNOP") == 0);
	struct bond full;
	assert(bond_snapshot(0, &full) == 0);
	assert(strlen(full.serial) == BOND_SERIAL_LEN);
	assert(bond_set(1, 3, 4, NULL) == 0);
}

static void test_transactions(void)
{
	reset_all();
	assert(bond_init() == 0);
	save_error = -EIO;
	assert(bond_import(0, record_a, empty_record) == -EIO);
	assert_record(0, empty_record);
	assert(!flash_present[0]);
	save_error = 0;
	assert(bond_import(0, record_a, empty_record) == 0);
	int before = writes;
	assert(bond_set(0, 0x11223344, 0x55667788, "SYNTHETIC-A") == 0);
	assert(writes == before);
	assert(bond_set(0, 1, 2, "DIFFERENT") == -EAGAIN);
	assert(bond_import(0, record_b, empty_record) == -EAGAIN);
	assert(bond_clear_if(0, record_b) == -EAGAIN);
	assert(bond_import(1, record_a, empty_record) == -EAGAIN);
	assert(writes == before);
	save_error = -ENOSPC;
	assert(bond_import(0, record_b, record_a) == -ENOSPC);
	assert_record(0, record_a);
	assert(bond_clear(0) == -ENOSPC);
	assert_record(0, record_a);
	assert(memcmp(flash_records[0], record_a, BOND_RECORD_LEN) == 0);
	save_error = 0;
	assert(bond_import(0, record_b, record_a) == 0);
	assert_record(0, record_b);
	assert(bond_clear_if(0, record_b) == 0);
	assert_record(0, empty_record);
	before = writes;
	assert(bond_clear(0) == 0 && writes == before);
}

static void test_load_errors(void)
{
	const char *keys[] = {"0junk", "0/nested", "-1", "4", "00", "", NULL};

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		reset_all();
		flash_present[0] = true;
		memcpy(flash_records[0], record_a, BOND_RECORD_LEN);
		inject_bad_load = true;
		bad_key = keys[i];
		bad_length = BOND_RECORD_LEN;
		assert(bond_init() == -EINVAL);
		assert(!bond_storage_ready());
		assert_record(0, empty_record);
		assert(bond_clear(0) == -EACCES);
	}
	for (size_t len = 23; len <= 25; len += 2) {
		reset_all();
		inject_bad_load = true;
		bad_key = "0";
		bad_length = len;
		assert(bond_init() == -EINVAL);
	}
	for (int length = -1; length <= 23; length += 24) {
		reset_all();
		flash_present[0] = true;
		short_read = length;
		assert(bond_init() == -EIO);
		assert(!bond_storage_ready());
	}
	reset_all();
	flash_present[0] = true;
	memcpy(flash_records[0], record_a, BOND_RECORD_LEN);
	flash_records[0][10] = 0;
	assert(bond_init() == -EINVAL && !bond_storage_ready());
	memcpy(flash_records[0], record_a, BOND_RECORD_LEN);
	assert(bond_init() == 0);
	assert_record(0, record_a);
	reset_all();
	inject_bad_load = true;
	bad_key = "0";
	bad_length = 0;
	assert(bond_init() == 0);
	assert_record(0, empty_record);
}

static void *paused_writer(void *unused)
{
	assert(bond_import(0, record_b, record_a) == 0);
	return NULL;
}

static void *stress_writer(void *unused)
{
	for (int i = 0; i < 2000; i++) {
		assert(bond_import(0, record_a, record_b) == 0);
		assert(bond_clear_if(0, record_a) == 0);
		assert(bond_import(0, record_b, empty_record) == 0);
	}
	atomic_store(&writer_done, true);
	return NULL;
}

static void test_concurrency(void)
{
	pthread_t thread;
	uint8_t record[BOND_RECORD_LEN];
	struct bond snapshot;
	int reads = 0;

	reset_all();
	assert(bond_init() == 0);
	assert(bond_import(0, record_a, empty_record) == 0);
	atomic_store(&pause_save, true);
	assert(pthread_create(&thread, NULL, paused_writer, NULL) == 0);
	while (!atomic_load(&save_entered)) {
		sched_yield();
	}
	for (int i = 0; i < 10000; i++) {
		assert_record(0, record_a);
	}
	atomic_store(&release_save, true);
	assert(pthread_join(thread, NULL) == 0);
	assert_record(0, record_b);
	atomic_store(&pause_save, false);
	assert(pthread_create(&thread, NULL, stress_writer, NULL) == 0);
	do {
		assert(bond_snapshot(0, &snapshot) == 0);
		encode(&snapshot, record);
		assert(memcmp(record, record_a, BOND_RECORD_LEN) == 0 ||
		       memcmp(record, record_b, BOND_RECORD_LEN) == 0 ||
		       memcmp(record, empty_record, BOND_RECORD_LEN) == 0);
		assert(snapshot.used == (memcmp(record, empty_record, BOND_RECORD_LEN) != 0));
		reads++;
	} while (!atomic_load(&writer_done) || reads < 10000);
	assert(pthread_join(thread, NULL) == 0);
}

static void test_console(void)
{
	const char *bad_slots[] = {"", "-1", "+0", "00", "4", "0junk", " 0", "4294967296"};
	const char *bad_ids[] = {"", "-1", "+1", " 1", "100000000", "0x100000000",
				 "0x", "1junk", "000000000"};
	uint32_t id = 0;
	int slot = -1;

	reset_all();
	assert(bond_init() == 0);
	for (size_t i = 0; i < sizeof(bad_slots) / sizeof(bad_slots[0]); i++) {
		assert(!parse_slot(bad_slots[i], &slot));
	}
	for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
		assert(!parse_u32(bad_ids[i], &id));
	}
	assert(parse_u32("FFFFFFFF", &id) && id == UINT32_MAX);
	assert(parse_u32("0x11223344", &id) && id == 0x11223344);
	char *set[] = {"set", "0", "11223344", "55667788", "SYNTHETIC-A"};
	assert(cmd_pair_set(&test_shell, 5, set) == 0);
	char *bad_clear[] = {"clear", "nonsense"};
	assert(cmd_pair_clear(&test_shell, 2, bad_clear) == -EINVAL);
	assert_record(0, record_a);
	char *bad_watch[] = {"watch", "invalid"};
	assert(cmd_watch(&test_shell, 2, bad_watch) == -EINVAL);
	assert(!watch_enabled);
	char hex_a[49], hex_b[49];
	record_hex(record_a, hex_a);
	record_hex(record_b, hex_b);
	char *import[] = {"import", "request_1", "0", hex_b, hex_a};
	assert(cmd_setup_import(&test_shell, 5, import) == 0);
	assert_record(0, record_b);
	assert(cmd_setup_import(&test_shell, 5, import) == -EAGAIN);
	char *clear[] = {"clear", "request_2", "0", hex_b};
	assert(cmd_setup_clear(&test_shell, 4, clear) == 0);
	assert_record(0, empty_record);
	reset_output();
	char *bad_request[] = {"info", "bad\"request"};
	assert(cmd_setup_info(&test_shell, 2, bad_request) == -EINVAL);
	assert(strstr(output, "bad\"request") == NULL);
	assert(strstr(output, "\"error\":\"invalid_argument\"") != NULL);
	for (int i = 0; i < BOND_SLOTS; i++) {
		assert(bond_set(i, 0x11110000 + i, 0x55550000 + i, "SYNTHETIC") == 0);
	}
	maximum_stats = true;
	reset_output();
	char *status[] = {"status", "0123456789abcdef"};
	assert(cmd_setup_status(&test_shell, 2, status) == 0);
	assert(strlen(output) - 1 <= SETUP_LINE_MAX);
	assert(strstr(output, "SYNTHETIC") == NULL);
	assert(strstr(output, "receiver") == NULL);
	assert(strstr(output, "controller") == NULL);
	assert(strstr(output, "4294967295") != NULL);
}

int main(int argc, char **argv)
{
	assert(argc == 2);
	if (strcmp(argv[1], "storage") == 0) {
		test_storage();
	} else if (strcmp(argv[1], "validation") == 0) {
		test_validation();
	} else if (strcmp(argv[1], "transactions") == 0) {
		test_transactions();
	} else if (strcmp(argv[1], "load-errors") == 0) {
		test_load_errors();
	} else if (strcmp(argv[1], "concurrency") == 0) {
		test_concurrency();
	} else if (strcmp(argv[1], "console") == 0) {
		test_console();
	} else {
		reset_all();
		assert(bond_init() == 0);
		char *args[] = {argv[1], "0123456789abcdef"};

		if (strcmp(argv[1], "info") == 0) {
			assert(cmd_setup_info(&test_shell, 2, args) == 0);
		} else if (strcmp(argv[1], "status") == 0) {
			assert(bond_import(0, record_a, empty_record) == 0);
			assert(cmd_setup_status(&test_shell, 2, args) == 0);
		} else {
			assert(strcmp(argv[1], "export") == 0);
			assert(bond_import(0, record_a, empty_record) == 0);
			assert(bond_import(1, record_b, empty_record) == 0);
			assert(cmd_setup_export(&test_shell, 2, args) == 0);
		}
		fputs(output, stdout);
	}
	return 0;
}
