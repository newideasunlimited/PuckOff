/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

static inline uint32_t sys_get_le32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static inline void sys_put_le32(uint32_t value, uint8_t *bytes)
{
	for (int i = 0; i < 4; i++) {
		bytes[i] = (uint8_t)(value >> (i * 8));
	}
}
