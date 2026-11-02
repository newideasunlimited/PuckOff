/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

struct radio_mpsl_stats {
	uint32_t requested;
	uint32_t granted;
	uint32_t denied;
	uint32_t deferred;
	uint32_t cutoffs;
	uint32_t forced_resets;
	uint32_t completion_timeouts;
	uint32_t invalid_signals;
	uint32_t max_call_us;
};

void radio_mpsl_get_stats(struct radio_mpsl_stats *out);
