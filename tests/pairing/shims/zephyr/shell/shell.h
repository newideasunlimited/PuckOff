/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

struct shell {
	int unused;
};

void test_shell_print(const struct shell *sh, const char *format, ...);
#define shell_print test_shell_print
#define shell_error test_shell_print
#define SHELL_STATIC_SUBCMD_SET_CREATE(name, ...) enum { name = 0 }
#define SHELL_CMD_REGISTER(...)
#define SHELL_CMD_ARG_REGISTER(...)
