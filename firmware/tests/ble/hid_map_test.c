/* SPDX-License-Identifier: Apache-2.0 */
#include "../../src/ble/hid_map.h"
#include "../../src/gamepad.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Walk the short items of a report descriptor, counting collections and IDs. */
int main(void)
{
	size_t i = 0;
	int depth = 0;
	int ids = 0;
	uint8_t seen_ids[8] = { 0 };

	/* The gamepad collection is the USB descriptor with Report ID 1 inserted. */
	assert(ble_hid_map_gamepad_len == gamepad_hid_desc_len + 2);
	assert(memcmp(ble_hid_map, gamepad_hid_desc, 6) == 0);
	assert(ble_hid_map[6] == 0x85 && ble_hid_map[7] == 0x01);
	assert(memcmp(ble_hid_map + 8, gamepad_hid_desc + 6, gamepad_hid_desc_len - 6) == 0);

	while (i < ble_hid_map_len) {
		uint8_t prefix = ble_hid_map[i];
		size_t size = prefix & 0x03;

		assert(prefix != 0xFE); /* no long items */
		size = size == 3 ? 4 : size;
		assert(i + 1 + size <= ble_hid_map_len);
		if (prefix == 0xA1) {
			depth++;
		} else if (prefix == 0xC0) {
			assert(depth > 0);
			depth--;
		} else if (prefix == 0x85) {
			assert(depth == 1);
			assert(ble_hid_map[i + 1] >= 1 && ble_hid_map[i + 1] <= 4);
			assert(!seen_ids[ble_hid_map[i + 1]]);
			seen_ids[ble_hid_map[i + 1]] = 1;
			ids++;
		}
		i += 1 + size;
	}
	assert(depth == 0 && ids == 4);
	assert(i == ble_hid_map_len);
	assert(ble_hid_map_len < 512); /* fits the HID Report Map characteristic */

	puts("hid_map: gamepad section matches USB, four report IDs, balanced collections");
	return 0;
}
