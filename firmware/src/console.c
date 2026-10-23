/* SPDX-License-Identifier: Apache-2.0 */
/* Setup commands on the USB serial port. Type "help" for the list. */
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "bond.h"
#include "link.h"

static bool parse_u32(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v = strtoul(s, &end, 16);

	if (*s == '\0' || *end != '\0') {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static int cmd_pair_show(const struct shell *sh, size_t argc, char **argv)
{
	for (int i = 0; i < BOND_SLOTS; i++) {
		const struct bond *b = bond_get(i);

		if (b->used) {
			shell_print(sh, "slot %d: receiver %08X controller %08X serial %s", i,
				    b->receiver_id, b->controller_id, b->serial);
		} else {
			shell_print(sh, "slot %d: empty", i);
		}
	}
	return 0;
}

static int cmd_pair_set(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t rid, cid;
	int slot = atoi(argv[1]);
	int err;

	if (!parse_u32(argv[2], &rid) || !parse_u32(argv[3], &cid)) {
		shell_error(sh, "ids are hex, e.g. 1A2B3C4D");
		return -EINVAL;
	}
	err = bond_set(slot, rid, cid, argc > 4 ? argv[4] : "");
	if (err) {
		shell_error(sh, "failed (%d)", err);
		return err;
	}
	shell_print(sh, "saved slot %d", slot);
	return 0;
}

static int cmd_pair_clear(const struct shell *sh, size_t argc, char **argv)
{
	int err = bond_clear(atoi(argv[1]));

	if (err) {
		shell_error(sh, "failed (%d)", err);
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
	for (int i = 0; i < BOND_SLOTS; i++) {
		struct link_stats st;

		if (!bond_get(i)->used) {
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
