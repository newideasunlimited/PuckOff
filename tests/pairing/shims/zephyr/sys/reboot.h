/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <assert.h>

#define SYS_REBOOT_COLD 0
static inline void sys_reboot(int type)
{
	assert(!"Tests must never invoke bootloader or reboot commands");
}
