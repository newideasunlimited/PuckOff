/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <nrfx.h>

static inline void irq_enable(unsigned int irqn)
{
	(void)irqn;
	test_nvic_enabled++;
}
