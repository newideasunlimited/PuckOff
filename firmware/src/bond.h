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
bool bond_storage_ready(void);
/* Copies a coherent value without holding a lock during flash I/O. */
int bond_snapshot(int slot, struct bond *out);
/* Exports one coherent table. Empty slots are all-zero records. */
int bond_export(uint8_t records[BOND_SLOTS][BOND_RECORD_LEN]);
/* Both mutations compare the current record with expected before writing. */
int bond_import(int slot, const uint8_t record[BOND_RECORD_LEN],
		const uint8_t expected[BOND_RECORD_LEN]);
int bond_clear_if(int slot, const uint8_t expected[BOND_RECORD_LEN]);
/* Legacy recovery API: set refuses replacement; clear is explicitly destructive. */
int bond_set(int slot, uint32_t receiver_id, uint32_t controller_id, const char *serial);
int bond_clear(int slot);
