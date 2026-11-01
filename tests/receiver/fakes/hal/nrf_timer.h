/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { NRF_TIMER_CC_CHANNEL0, NRF_TIMER_CC_CHANNEL1, NRF_TIMER_CC_CHANNEL2 } nrf_timer_cc_channel_t;
typedef enum { NRF_TIMER_EVENT_COMPARE0, NRF_TIMER_EVENT_COMPARE1, NRF_TIMER_EVENT_COMPARE2 } nrf_timer_event_t;
typedef enum { NRF_TIMER_TASK_CAPTURE2 } nrf_timer_task_t;
#define NRF_TIMER_INT_COMPARE0_MASK (1U << 16)
#define NRF_TIMER_INT_COMPARE1_MASK (1U << 17)

/* Fake TIMER0: the slot clock, compare values, pending events and enabled interrupts. */
struct fake_timer {
	uint32_t now_us;
	uint32_t cc[3];
	bool event[3];
	uint32_t inten;
};
extern struct fake_timer test_timer;
#define MPSL_TIMER0 (&test_timer)

static inline void nrf_timer_task_trigger(struct fake_timer *t, nrf_timer_task_t task)
{
	(void)task;
	t->cc[2] = t->now_us;
}
static inline uint32_t nrf_timer_cc_get(struct fake_timer *t, nrf_timer_cc_channel_t ch)
{
	return t->cc[ch];
}
static inline void nrf_timer_cc_set(struct fake_timer *t, nrf_timer_cc_channel_t ch, uint32_t v)
{
	t->cc[ch] = v;
}
static inline nrf_timer_event_t nrf_timer_compare_event_get(nrf_timer_cc_channel_t ch)
{
	return (nrf_timer_event_t)ch;
}
static inline uint32_t nrf_timer_compare_int_get(nrf_timer_cc_channel_t ch)
{
	return NRF_TIMER_INT_COMPARE0_MASK << ch;
}
static inline void nrf_timer_event_clear(struct fake_timer *t, nrf_timer_event_t ev)
{
	t->event[ev] = false;
}
static inline bool nrf_timer_event_check(struct fake_timer *t, nrf_timer_event_t ev)
{
	return t->event[ev];
}
static inline void nrf_timer_int_enable(struct fake_timer *t, uint32_t mask)
{
	t->inten |= mask;
}
static inline void nrf_timer_int_disable(struct fake_timer *t, uint32_t mask)
{
	t->inten &= ~mask;
}
