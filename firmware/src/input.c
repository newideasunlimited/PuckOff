/* SPDX-License-Identifier: Apache-2.0 */
#include "input.h"
#include "radio.h"
#include <string.h>

#define REPORT_STATE_LEGACY 0x45
#define REPORT_STATE 0x42
#define RECORD_HID_REPORT 6
#define MIN_STATE_LEN 0x1E /* through right pad force */
#define FULL_STATE_LEN 0x2E /* through gyro */

static uint16_t u16le(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static int16_t s16le(const uint8_t *p)
{
	return (int16_t)u16le(p);
}

bool input_decode_report(const uint8_t *rep, size_t len, struct ctrl_state *out)
{
	if (rep == NULL || out == NULL || len < MIN_STATE_LEN ||
	    (rep[0] != REPORT_STATE_LEGACY && rep[0] != REPORT_STATE)) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->report_id = rep[0];
	out->seq = rep[1];
	out->buttons = (uint32_t)rep[2] | (uint32_t)rep[3] << 8 |
		       (uint32_t)rep[4] << 16 | (uint32_t)rep[5] << 24;
	out->lt = u16le(rep + 0x06);
	out->rt = u16le(rep + 0x08);
	out->lx = s16le(rep + 0x0A);
	out->ly = s16le(rep + 0x0C);
	out->rx = s16le(rep + 0x0E);
	out->ry = s16le(rep + 0x10);
	out->lpad_x = s16le(rep + 0x12);
	out->lpad_y = s16le(rep + 0x14);
	out->lpad_force = u16le(rep + 0x16);
	out->rpad_x = s16le(rep + 0x18);
	out->rpad_y = s16le(rep + 0x1A);
	out->rpad_force = u16le(rep + 0x1C);

	if (len >= FULL_STATE_LEN) {
		out->has_imu = true;
		out->imu_time_us = (uint32_t)rep[0x1E] | (uint32_t)rep[0x1F] << 8 |
				   (uint32_t)rep[0x20] << 16 | (uint32_t)rep[0x21] << 24;
		for (int i = 0; i < 3; i++) {
			out->accel[i] = s16le(rep + 0x22 + 2 * i);
			out->gyro[i] = s16le(rep + 0x28 + 2 * i);
		}
	}
	return true;
}

bool input_from_packet(const uint8_t *pkt, size_t size, struct ctrl_state *out)
{
	struct ctrl_state latest;
	size_t end;
	size_t i = 3;
	bool found = false;

	if (pkt == NULL || out == NULL || size < 3 || pkt[0] < 1 ||
	    pkt[0] > RADIO_MAX_PAYLOAD || (size_t)pkt[0] + 2 > size || pkt[2] != 0xF1) {
		return false;
	}
	end = (size_t)pkt[0] + 2;
	while (i < end) {
		if (end - i < 2) {
			return false;
		}
		uint8_t rlen = pkt[i];
		uint8_t rtype = pkt[i + 1];
		const uint8_t *val = pkt + i + 2;

		if (rlen == 0 || rlen > end - i - 2) {
			return false;
		}
		if (rtype == RECORD_HID_REPORT) {
			if (input_decode_report(val, rlen, &latest)) {
				found = true;
			} else if (val[0] == REPORT_STATE_LEGACY || val[0] == REPORT_STATE) {
				return false;
			}
		}
		i += (size_t)rlen + 2;
	}
	if (found) {
		*out = latest;
	}
	return found;
}
