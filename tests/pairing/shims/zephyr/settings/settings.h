/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stddef.h>
#include <sys/types.h>

typedef ssize_t (*settings_read_cb)(void *arg, void *data, size_t len);
int settings_subsys_init(void);
int settings_load_subtree(const char *subtree);
int settings_save_one(const char *key, const void *data, size_t len);

#define SETTINGS_STATIC_HANDLER_DEFINE(...)
