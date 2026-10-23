/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdatomic.h>
#include <stdbool.h>
typedef atomic_int atomic_t;
static inline int atomic_get(const atomic_t *value) { return atomic_load(value); }
static inline void atomic_set(atomic_t *value, int n) { atomic_store(value, n); }
static inline void atomic_clear(atomic_t *value) { atomic_store(value, 0); }
static inline bool atomic_cas(atomic_t *value, int expected, int desired)
{
	return atomic_compare_exchange_strong(value, &expected, desired);
}
