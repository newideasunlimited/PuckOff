/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BOND_SLOTS 4
#define BOND_SERIAL_LEN 16

/*
 * A pairing between this receiver and one controller. The wire format
 * (BOND_RECORD_LEN bytes) is the same one Valve's receiver uses, so a pairing
 * can be moved between receivers.
 */
struct bond {
	bool used;
	uint32_t receiver_id;
	uint32_t controller_id;
	char serial[BOND_SERIAL_LEN + 1];
};

#define BOND_RECORD_LEN (4 + 4 + BOND_SERIAL_LEN)

int bond_init(void);
const struct bond *bond_get(int slot);
int bond_set(int slot, uint32_t receiver_id, uint32_t controller_id, const char *serial);
int bond_clear(int slot);
