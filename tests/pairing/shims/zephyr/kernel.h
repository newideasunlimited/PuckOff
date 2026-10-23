/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct k_mutex {
	pthread_mutex_t mutex;
};

#define K_MUTEX_DEFINE(name) struct k_mutex name = {PTHREAD_MUTEX_INITIALIZER}
#define K_FOREVER 0
#define snprintk snprintf

static inline int k_mutex_lock(struct k_mutex *lock, int timeout)
{
	return pthread_mutex_lock(&lock->mutex);
}

static inline int k_mutex_unlock(struct k_mutex *lock)
{
	return pthread_mutex_unlock(&lock->mutex);
}

struct k_spinlock {
	atomic_bool locked;
};
typedef int k_spinlock_key_t;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	while (atomic_exchange_explicit(&lock->locked, true, memory_order_acquire)) {
		sched_yield();
	}
	return 0;
}

static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	atomic_store_explicit(&lock->locked, false, memory_order_release);
}

struct test_power_registers {
	uint32_t GPREGRET;
};
extern struct test_power_registers test_power;
#define NRF_POWER (&test_power)
