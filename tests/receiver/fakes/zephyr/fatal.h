/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
struct arch_esf {
	struct { uint32_t r0, r1, r2, r3, r12, lr, pc, xpsr; } basic;
};
#define K_ERR_CPU_EXCEPTION 0
#define K_ERR_KERNEL_OOPS 3
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf);
