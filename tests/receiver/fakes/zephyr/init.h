/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#define PRE_KERNEL_1 0
#define SYS_INIT(fn, level, prio) int (*const test_sys_init_##fn)(void) = fn
