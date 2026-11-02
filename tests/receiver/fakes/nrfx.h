/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <hal/nrf_radio.h>

/* Fake NVIC: records what the backend did with the RADIO interrupt line. */
#define RADIO_IRQn 1
extern unsigned int test_nvic_enabled, test_nvic_clear_pending;
static inline void NVIC_ClearPendingIRQ(int irqn)
{
	(void)irqn;
	test_nvic_clear_pending++;
}
