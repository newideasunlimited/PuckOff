/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define __aligned(n) __attribute__((aligned(n)))
#define __packed __attribute__((packed))
#define ARG_UNUSED(x) (void)(x)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define K_NO_WAIT 0
#define K_PRIO_PREEMPT(n) (n)
#define K_THREAD_STACK_DEFINE(name, size) static uint8_t name[size]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)

struct k_spinlock { atomic_bool locked; };
typedef int k_spinlock_key_t;
static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *lock)
{
	while (atomic_exchange(&lock->locked, true)) {}
	return 0;
}
static inline void k_spin_unlock(struct k_spinlock *lock, k_spinlock_key_t key)
{
	(void)key;
	atomic_store(&lock->locked, false);
}
#define K_SPINLOCK(lock) \
	for (bool _locked = (k_spin_lock(lock), true); _locked; \
	     k_spin_unlock(lock, 0), _locked = false)

struct k_thread { int unused; };
typedef struct k_thread *k_tid_t;
int64_t k_uptime_ticks(void);
uint32_t k_uptime_get_32(void);
static inline uint64_t k_ticks_to_us_floor64(uint64_t ticks)
{
	return ticks * 1000000U / CONFIG_SYS_CLOCK_TICKS_PER_SEC;
}
void k_msleep(uint32_t ms);
void k_usleep(uint32_t us);
k_tid_t k_thread_create(struct k_thread *thread, void *stack, size_t size,
		       void (*entry)(void *, void *, void *), void *a, void *b, void *c,
		       int priority, uint32_t options, int delay);
int k_thread_name_set(struct k_thread *thread, const char *name);
int printk(const char *format, ...);
