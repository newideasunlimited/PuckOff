/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

struct test_power_registers {
	uint32_t GPREGRET;
};
extern struct test_power_registers test_power;
#define NRF_POWER (&test_power)
