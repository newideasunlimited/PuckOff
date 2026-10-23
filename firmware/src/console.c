/* SPDX-License-Identifier: Apache-2.0 */
/* Setup commands on the USB serial port. Type "help" for the list. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "bond.h"
#include "link.h"

static bool parse_u32(const char *s, uint32_t *out)
{
	uint32_t value = 0;
	size_t digits;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	digits = strlen(s);
	if (digits == 0 || digits > 8) {
		return false;
	}
	for (size_t i = 0; i < digits; i++) {
		unsigned int nibble;

		if (s[i] >= '0' && s[i] <= '9') {
			nibble = s[i] - '0';
		} else if (s[i] >= 'a' && s[i] <= 'f') {
			nibble = s[i] - 'a' + 10;
		} else if (s[i] >= 'A' && s[i] <= 'F') {
			nibble = s[i] - 'A' + 10;
		} else {
			return false;
		}
		value = (value << 4) | nibble;
	}
	*out = value;
	return true;
}

static bool parse_slot(const char *s, int *out)
{
	if (s[0] < '0' || s[0] >= '0' + BOND_SLOTS || s[1] != '\0') {
		return false;
	}
	*out = s[0] - '0';
	return true;
}

static int cmd_pair_show(const struct shell *sh, size_t argc, char **argv)
{
	if (!bond_storage_ready()) {
		shell_error(sh, "pairing storage unavailable");
		return -EACCES;
	}
	for (int i = 0; i < BOND_SLOTS; i++) {
		struct bond b;

		bond_snapshot(i, &b);
		if (b.used) {
			shell_print(sh, "slot %d: receiver %08X controller %08X serial %s", i,
				    b.receiver_id, b.controller_id, b.serial);
		} else {
			shell_print(sh, "slot %d: empty", i);
		}
	}
	return 0;
}

static int cmd_pair_set(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t rid, cid;
	int slot;
	int err;

	if (!parse_slot(argv[1], &slot) || !parse_u32(argv[2], &rid) ||
	    !parse_u32(argv[3], &cid)) {
		shell_error(sh, "slot must be 0-3; ids must be 1-8 hex digits");
		return -EINVAL;
	}
	err = bond_set(slot, rid, cid, argc > 4 ? argv[4] : "");
	if (err) {
		shell_error(sh, "not saved (%d); occupied/duplicate slots are never overwritten",
			    err);
		return err;
	}
	shell_print(sh, "saved slot %d", slot);
	return 0;
}

static int cmd_pair_clear(const struct shell *sh, size_t argc, char **argv)
{
	int slot, err;

	if (!parse_slot(argv[1], &slot)) {
		shell_error(sh, "slot must be 0-3");
		return -EINVAL;
	}
	err = bond_clear(slot);
	if (err) {
		shell_error(sh, "not cleared (%d)", err);
	} else {
		shell_print(sh, "cleared slot %d", slot);
	}
	return err;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pair_cmds,
	SHELL_CMD_ARG(show, NULL, "List paired controllers", cmd_pair_show, 1, 0),
	SHELL_CMD_ARG(set, NULL, "set <slot> <receiver-id> <controller-id> [serial]",
		      cmd_pair_set, 4, 1),
	SHELL_CMD_ARG(clear, NULL, "clear <slot>", cmd_pair_clear, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pair, &pair_cmds, "Controller pairing", NULL);

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	if (!bond_storage_ready()) {
		shell_error(sh, "pairing storage unavailable");
		return -EACCES;
	}
	for (int i = 0; i < BOND_SLOTS; i++) {
		struct link_stats st;
		struct bond b;

		bond_snapshot(i, &b);
		if (!b.used) {
			continue;
		}
		int err = link_get_stats(i, &st);

		if (err) {
			shell_error(sh, "slot %d: status failed (%d)", i, err);
			return err;
		}
		shell_print(sh,
			    "slot %d: %s rssi -%u dBm polls %u replies %u reports %u crc-errors %u "
			    "invalid %u radio-errors %u deferred %u",
			    i, st.connected ? "CONNECTED" : (st.powered_off ? "off" : "searching"),
			    st.rssi, st.polls, st.replies, st.reports, st.crc_errors,
			    st.invalid_packets, st.radio_errors, st.deferred);
	}
	return 0;
}
SHELL_CMD_REGISTER(status, NULL, "Link status", cmd_status);

static int cmd_watch(const struct shell *sh, size_t argc, char **argv)
{
	extern void main_set_watch(bool on);
	bool on = argc < 2 || strcmp(argv[1], "off") != 0;

	if (argc > 1 && strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
		shell_error(sh, "use watch on or watch off");
		return -EINVAL;
	}
	main_set_watch(on);
	link_set_verbose(on);
	shell_print(sh, "input watch %s", on ? "on" : "off");
	return 0;
}
SHELL_CMD_ARG_REGISTER(watch, NULL, "watch [on|off]: print controller input", cmd_watch, 1, 1);

static int cmd_bootloader(const struct shell *sh, size_t argc, char **argv)
{
	/* Magic value the Adafruit UF2 bootloader checks to stay in DFU mode. */
	NRF_POWER->GPREGRET = 0x57;
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
SHELL_CMD_REGISTER(bootloader, NULL, "Reboot into the UF2 drive for flashing", cmd_bootloader);

/*
 * One bounded line per response; request ids distinguish responses from shell
 * echo, asynchronous logs and stale replies. Never include secrets in errors.
 */
#define SETUP_PREFIX "PUCKOFF/1 "
#define SETUP_LINE_MAX 1024

static bool valid_request_id(const char *id)
{
	size_t len = strlen(id);

	if (len == 0 || len > 16) {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = id[i];

		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

static const char *request_id(size_t argc, char **argv)
{
	return argc > 1 && valid_request_id(argv[1]) ? argv[1] : "";
}

static int setup_error(const struct shell *sh, const char *id, const char *op, int err)
{
	const char *code = err == -EINVAL ? "invalid_argument" :
			   err == -EACCES ? "storage_unavailable" :
			   err == -EAGAIN ? "conflict" : "storage_io";

	shell_print(sh, SETUP_PREFIX "{\"id\":\"%s\",\"op\":\"%s\",\"ok\":false,"
		    "\"error\":\"%s\"}", id, op, code);
	return err;
}

static bool append(char *buf, size_t size, size_t *pos, const char *format, ...)
{
	va_list args;
	int len;

	va_start(args, format);
	len = vsnprintf(buf + *pos, size - *pos, format, args);
	va_end(args);
	if (len < 0 || (size_t)len >= size - *pos) {
		return false;
	}
	*pos += (size_t)len;
	return true;
}

static int cmd_setup_info(const struct shell *sh, size_t argc, char **argv)
{
	const char *id = request_id(argc, argv);

	if (argc != 2 || !*id) {
		return setup_error(sh, id, "info", -EINVAL);
	}
	shell_print(sh, SETUP_PREFIX "{\"id\":\"%s\",\"op\":\"info\",\"ok\":true,"
		    "\"protocol\":1,\"product\":\"PuckOff Receiver\",\"firmware\":\"dev\","
		    "\"slots\":%d,\"record_bytes\":%d,"
		    "\"capabilities\":[\"status\",\"export\",\"import\",\"clear\"],"
		    "\"native_pairing\":false}", id, BOND_SLOTS, BOND_RECORD_LEN);
	return 0;
}

static int cmd_setup_status(const struct shell *sh, size_t argc, char **argv)
{
	const char *id = request_id(argc, argv);
	char line[SETUP_LINE_MAX + 1];
	size_t pos = 0;
	bool fits;

	if (argc != 2 || !*id) {
		return setup_error(sh, id, "status", -EINVAL);
	}
	fits = append(line, sizeof(line), &pos, SETUP_PREFIX
		      "{\"id\":\"%s\",\"op\":\"status\",\"ok\":true,"
		      "\"storage_ready\":%s,\"slots\":[",
		      id, bond_storage_ready() ? "true" : "false");
	for (int i = 0; i < BOND_SLOTS && fits; i++) {
		struct bond b;
		struct link_stats st = {0};

		bond_snapshot(i, &b);
		if (b.used) {
			int err = link_get_stats(i, &st);

			if (err) {
				return setup_error(sh, id, "status", err);
			}
		}
		fits = append(line, sizeof(line), &pos,
			      "%s{\"slot\":%d,\"used\":%s,\"connected\":%s,"
			      "\"powered_off\":%s,\"rssi\":%d,\"polls\":%u,"
			      "\"replies\":%u,\"reports\":%u,\"crc_errors\":%u}",
			      i ? "," : "", i, b.used ? "true" : "false",
			      st.connected ? "true" : "false",
			      st.powered_off ? "true" : "false", -(int)st.rssi,
			      st.polls, st.replies, st.reports, st.crc_errors);
	}
	if (!fits || !append(line, sizeof(line), &pos, "]}")) {
		return setup_error(sh, id, "status", -EOVERFLOW);
	}
	shell_print(sh, "%s", line);
	return 0;
}

static void record_hex(const uint8_t rec[BOND_RECORD_LEN], char hex[BOND_RECORD_LEN * 2 + 1])
{
	static const char digits[] = "0123456789abcdef";

	for (int i = 0; i < BOND_RECORD_LEN; i++) {
		hex[i * 2] = digits[rec[i] >> 4];
		hex[i * 2 + 1] = digits[rec[i] & 0x0f];
	}
	hex[BOND_RECORD_LEN * 2] = '\0';
}

static bool parse_record(const char *hex, uint8_t rec[BOND_RECORD_LEN])
{
	if (strlen(hex) != BOND_RECORD_LEN * 2) {
		return false;
	}
	for (int i = 0; i < BOND_RECORD_LEN; i++) {
		char byte[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
		uint32_t value;

		if (!parse_u32(byte, &value)) {
			return false;
		}
		rec[i] = (uint8_t)value;
	}
	return true;
}

static bool parse_expected(const char *text, uint8_t rec[BOND_RECORD_LEN])
{
	if (strcmp(text, "empty") == 0) {
		memset(rec, 0, BOND_RECORD_LEN);
		return true;
	}
	return parse_record(text, rec);
}

static int cmd_setup_export(const struct shell *sh, size_t argc, char **argv)
{
	const char *id = request_id(argc, argv);
	uint8_t records[BOND_SLOTS][BOND_RECORD_LEN];
	char line[SETUP_LINE_MAX + 1];
	size_t pos = 0;
	bool fits;
	int err;

	if (argc != 2 || !*id) {
		return setup_error(sh, id, "export", -EINVAL);
	}
	err = bond_export(records);
	if (err) {
		return setup_error(sh, id, "export", err);
	}
	fits = append(line, sizeof(line), &pos, SETUP_PREFIX
		      "{\"id\":\"%s\",\"op\":\"export\",\"ok\":true,\"slots\":[", id);
	for (int i = 0; i < BOND_SLOTS && fits; i++) {
		char hex[BOND_RECORD_LEN * 2 + 1];
		bool used = false;

		record_hex(records[i], hex);
		for (int j = 0; j < BOND_RECORD_LEN; j++) {
			used |= records[i][j] != 0;
		}
		fits = append(line, sizeof(line), &pos, "%s{\"slot\":%d,\"record\":%s%s%s}",
			      i ? "," : "", i, used ? "\"" : "",
			      used ? hex : "null", used ? "\"" : "");
	}
	if (!fits || !append(line, sizeof(line), &pos, "]}")) {
		return setup_error(sh, id, "export", -EOVERFLOW);
	}
	shell_print(sh, "%s", line);
	return 0;
}

static int cmd_setup_import(const struct shell *sh, size_t argc, char **argv)
{
	const char *id = request_id(argc, argv);
	uint8_t record[BOND_RECORD_LEN], expected[BOND_RECORD_LEN];
	int slot, err;

	if (argc != 5 || !*id || !parse_slot(argv[2], &slot) ||
	    !parse_record(argv[3], record) || !parse_expected(argv[4], expected)) {
		return setup_error(sh, id, "import", -EINVAL);
	}
	err = bond_import(slot, record, expected);
	if (err) {
		return setup_error(sh, id, "import", err);
	}
	shell_print(sh, SETUP_PREFIX
		    "{\"id\":\"%s\",\"op\":\"import\",\"ok\":true,\"slot\":%d}", id, slot);
	return 0;
}

static int cmd_setup_clear(const struct shell *sh, size_t argc, char **argv)
{
	const char *id = request_id(argc, argv);
	uint8_t expected[BOND_RECORD_LEN];
	int slot, err;

	if (argc != 4 || !*id || !parse_slot(argv[2], &slot) ||
	    !parse_expected(argv[3], expected)) {
		return setup_error(sh, id, "clear", -EINVAL);
	}
	err = bond_clear_if(slot, expected);
	if (err) {
		return setup_error(sh, id, "clear", err);
	}
	shell_print(sh, SETUP_PREFIX
		    "{\"id\":\"%s\",\"op\":\"clear\",\"ok\":true,\"slot\":%d}", id, slot);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(setup_cmds,
	SHELL_CMD_ARG(info, NULL, "info <request-id>", cmd_setup_info, 1, 1),
	SHELL_CMD_ARG(status, NULL, "status <request-id>", cmd_setup_status, 1, 1),
	SHELL_CMD_ARG(export, NULL, "export <request-id>: private backup", cmd_setup_export, 1, 1),
	SHELL_CMD_ARG(import, NULL, "import <request-id> <slot> <record-hex> <expected-hex|empty>",
		      cmd_setup_import, 1, 4),
	SHELL_CMD_ARG(clear, NULL, "clear <request-id> <slot> <expected-hex|empty>",
		      cmd_setup_clear, 1, 3),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(setup, &setup_cmds, "Versioned host management", NULL);
