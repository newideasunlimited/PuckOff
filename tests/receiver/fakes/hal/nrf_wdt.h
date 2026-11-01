/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t RUNSTATUS;
	uint32_t CRV;
	uint32_t RREN;
	uint32_t CONFIG;
	uint32_t RR[8]; /* counts reload requests instead of storing the magic value */
	uint32_t starts;
} NRF_WDT_Type;

extern NRF_WDT_Type test_wdt;
#define NRF_WDT (&test_wdt)

typedef enum { NRF_WDT_TASK_START } nrf_wdt_task_t;
typedef enum { NRF_WDT_RR0 } nrf_wdt_rr_register_t;
#define NRF_WDT_BEHAVIOUR_RUN_SLEEP_MASK 0x1u
#define NRF_WDT_BEHAVIOUR_RUN_HALT_MASK 0x8u

static inline void nrf_wdt_behaviour_set(NRF_WDT_Type *p_reg, uint32_t mask)
{
	p_reg->CONFIG = mask;
}
static inline void nrf_wdt_reload_value_set(NRF_WDT_Type *p_reg, uint32_t reload_value)
{
	p_reg->CRV = reload_value;
}
static inline void nrf_wdt_reload_request_enable(NRF_WDT_Type *p_reg, nrf_wdt_rr_register_t rr)
{
	p_reg->RREN |= 1u << rr;
}
static inline void nrf_wdt_task_trigger(NRF_WDT_Type *p_reg, nrf_wdt_task_t task)
{
	(void)task;
	p_reg->RUNSTATUS = 1;
	p_reg->starts++;
}
static inline bool nrf_wdt_started_check(const NRF_WDT_Type *p_reg)
{
	return p_reg->RUNSTATUS != 0;
}
static inline void nrf_wdt_reload_request_set(NRF_WDT_Type *p_reg, nrf_wdt_rr_register_t rr)
{
	p_reg->RR[rr]++;
}
