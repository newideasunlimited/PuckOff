/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

typedef struct {
	uint32_t RESETREAS;
	uint32_t GPREGRET[2];
} NRF_POWER_Type;

extern NRF_POWER_Type test_power;
#define NRF_POWER (&test_power)

#define NRF_POWER_RESETREAS_RESETPIN_MASK 0x1u
#define NRF_POWER_RESETREAS_DOG_MASK 0x2u
#define NRF_POWER_RESETREAS_SREQ_MASK 0x4u
#define NRF_POWER_RESETREAS_LOCKUP_MASK 0x8u

static inline uint32_t nrf_power_resetreas_get(const NRF_POWER_Type *p_reg)
{
	return p_reg->RESETREAS;
}
static inline void nrf_power_resetreas_clear(NRF_POWER_Type *p_reg, uint32_t mask)
{
	p_reg->RESETREAS &= ~mask;
}
static inline void nrf_power_gpregret_set(NRF_POWER_Type *p_reg, uint32_t reg_num, uint32_t val)
{
	p_reg->GPREGRET[reg_num] = val;
}
static inline uint32_t nrf_power_gpregret_get(const NRF_POWER_Type *p_reg, uint32_t reg_num)
{
	return p_reg->GPREGRET[reg_num];
}
