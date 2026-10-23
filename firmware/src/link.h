/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "input.h"

struct link_stats {
	bool connected;
	bool powered_off;
	uint8_t rssi;
	uint32_t polls;
	uint32_t replies;
	uint32_t reports;
	uint32_t crc_errors;
	uint32_t last_reply_ms;
};

/* Called from the radio thread whenever fresh input arrives or a link drops. */
typedef void (*link_input_cb)(int slot, const struct ctrl_state *state);
typedef void (*link_lost_cb)(int slot);

void link_start(link_input_cb on_input, link_lost_cb on_lost);
void link_get_stats(int slot, struct link_stats *out);
bool link_any_connected(void);
void link_set_verbose(bool on);
