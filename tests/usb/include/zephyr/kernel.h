/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <zephyr/toolchain.h>

struct k_spinlock {
	atomic_bool held;
};

static inline void test_lock(struct k_spinlock *lock)
{
	while (atomic_exchange_explicit(&lock->held, true, memory_order_acquire)) {
	}
}

static inline void test_unlock(struct k_spinlock *lock)
{
	atomic_store_explicit(&lock->held, false, memory_order_release);
}

#define K_SPINLOCK(lock) \
	for (bool held = (test_lock(lock), true); held; test_unlock(lock), held = false)

struct k_work {
	void (*handler)(struct k_work *);
};

struct k_work_delayable {
	struct k_work work;
};

#define K_WORK_DELAYABLE_DEFINE(name, fn) \
	struct k_work_delayable name = { .work = { .handler = fn } }
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)

int64_t k_uptime_get(void);
int k_work_reschedule(struct k_work_delayable *work, int64_t delay_ms);
int k_work_schedule(struct k_work_delayable *work, int64_t delay_ms);
