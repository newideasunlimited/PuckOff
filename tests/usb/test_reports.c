/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gamepad.h"
#include "usb_switch.h"

struct hid_field {
	unsigned int size, count, page, unit, flags, offset, kind;
	int32_t min, max, physical_min, physical_max;
	unsigned int usages[16], usage_count, usage_min, usage_max;
};

struct hid_layout {
	unsigned int input_bits, output_bits, count;
	struct hid_field fields[16];
};

static int32_t signed_item(uint32_t value, unsigned int bytes)
{
	if (bytes == 1) {
		return (int8_t)value;
	}
	if (bytes == 2) {
		return (int16_t)value;
	}
	return (int32_t)value;
}

/* Parse the actual compiled descriptors, not a duplicated expected byte array. */
static struct hid_layout parse(const uint8_t *desc, size_t len)
{
	struct hid_layout out = {0};
	struct hid_field state = {0};
	int collections = 0;

	for (size_t i = 0; i < len;) {
		uint8_t prefix = desc[i++];
		unsigned int bytes = prefix & 3;
		unsigned int type = (prefix >> 2) & 3;
		unsigned int tag = prefix >> 4;
		uint32_t value = 0;

		assert(prefix != 0xFE);
		bytes = bytes == 3 ? 4 : bytes;
		assert(i + bytes <= len);
		for (unsigned int j = 0; j < bytes; j++) {
			value |= (uint32_t)desc[i++] << (j * 8);
		}
		if (type == 1) {
			switch (tag) {
			case 0: state.page = value; break;
			case 1: state.min = signed_item(value, bytes); break;
			case 2:
				state.max = state.min < 0 ? signed_item(value, bytes) : (int32_t)value;
				break;
			case 3: state.physical_min = signed_item(value, bytes); break;
			case 4:
				state.physical_max = state.physical_min < 0 ?
					signed_item(value, bytes) : (int32_t)value;
				break;
			case 6: state.unit = value; break;
			case 7: state.size = value; break;
			case 8: assert(!"Report IDs would change the public wire format"); break;
			case 9: state.count = value; break;
			default: assert(!"Unexpected HID global item");
			}
		} else if (type == 2) {
			switch (tag) {
			case 0:
				assert(state.usage_count < 16);
				state.usages[state.usage_count++] = value;
				break;
			case 1: state.usage_min = value; break;
			case 2: state.usage_max = value; break;
			default: assert(!"Unexpected HID local item");
			}
		} else if (type == 0) {
			if (tag == 8 || tag == 9) {
				unsigned int *bits = tag == 8 ? &out.input_bits : &out.output_bits;

				assert(out.count < 16);
				state.flags = value;
				state.offset = *bits;
				state.kind = tag;
				out.fields[out.count++] = state;
				*bits += state.size * state.count;
			} else if (tag == 10) {
				assert(state.page == 1 && state.usages[0] == 5);
				collections++;
			} else if (tag == 12) {
				collections--;
			} else {
				assert(!"Unexpected HID main item");
			}
			state.usage_count = state.usage_min = state.usage_max = 0;
			memset(state.usages, 0, sizeof(state.usages));
		} else {
			assert(!"Reserved HID item");
		}
	}
	assert(collections == 0);
	return out;
}

static void test_descriptors(void)
{
	struct hid_layout generic = parse(gamepad_hid_desc, gamepad_hid_desc_len);
	struct hid_layout wired = parse(switch_hid_desc, switch_hid_desc_len);
	const unsigned int axis_usages[] = {0x30, 0x31, 0x32, 0x35};

	assert(sizeof(struct gamepad_report) == 13);
	assert(generic.input_bits == 104 && generic.output_bits == 0 && generic.count == 5);
	assert(generic.fields[0].count == 16 && generic.fields[0].size == 1);
	assert(generic.fields[0].page == 9 && generic.fields[0].usage_min == 1 &&
	       generic.fields[0].usage_max == 16 && generic.fields[0].flags == 2);
	assert(generic.fields[1].offset == 16 && generic.fields[1].size == 4);
	assert(generic.fields[1].usages[0] == 0x39 && generic.fields[1].max == 7);
	assert(generic.fields[1].flags == 0x42 && generic.fields[1].unit == 0x14);
	assert(generic.fields[1].physical_max == 315);
	assert(generic.fields[2].size == 4 && (generic.fields[2].flags & 1));
	assert(generic.fields[3].offset == 24 && generic.fields[3].size == 16 &&
	       generic.fields[3].count == 4);
	assert(memcmp(generic.fields[3].usages, axis_usages, sizeof(axis_usages)) == 0);
	assert(generic.fields[3].min == -32767 && generic.fields[3].max == 32767);
	assert(generic.fields[3].physical_min == 0 && generic.fields[3].physical_max == 0);
	assert(generic.fields[3].unit == 0 && generic.fields[4].unit == 0);
	assert(generic.fields[4].offset == 88 && generic.fields[4].size == 8 &&
	       generic.fields[4].count == 2);
	assert(generic.fields[4].usages[0] == 0x33 && generic.fields[4].usages[1] == 0x34);
	assert(generic.fields[4].min == 0 && generic.fields[4].max == 255);

	assert(wired.input_bits == 64 && wired.output_bits == 64 && wired.count == 7);
	assert(wired.fields[0].count == 13 && wired.fields[0].usage_max == 13);
	assert(wired.fields[1].offset == 13 && wired.fields[1].count == 3 &&
	       (wired.fields[1].flags & 1));
	assert(wired.fields[2].offset == 16 && wired.fields[2].flags == 0x42);
	assert(wired.fields[2].usages[0] == 0x39 && wired.fields[2].max == 7);
	assert(wired.fields[3].size == 4 && (wired.fields[3].flags & 1));
	assert(wired.fields[4].offset == 24 && wired.fields[4].size == 8 &&
	       wired.fields[4].count == 4 && wired.fields[4].max == 255);
	assert(wired.fields[4].physical_max == 255 && wired.fields[4].unit == 0);
	assert(memcmp(wired.fields[4].usages, axis_usages, sizeof(axis_usages)) == 0);
	assert(wired.fields[5].offset == 56 && wired.fields[5].page == 0xFF00 &&
	       wired.fields[5].usages[0] == 0x20 && wired.fields[5].size == 8);
	assert(wired.fields[6].kind == 9 && wired.fields[6].page == 0xFF00 &&
	       wired.fields[6].usages[0] == 0x2621 && wired.fields[6].count == 8);
}

static void test_controller_mapping(void)
{
	static const uint32_t ctrl_buttons[] = {
		BTN_A, BTN_B, BTN_X, BTN_Y, BTN_LB, BTN_RB, BTN_VIEW, BTN_MENU,
		BTN_L3, BTN_R3, BTN_STEAM, BTN_QAM, BTN_L4, BTN_R4, BTN_L5, BTN_R5,
	};
	static const uint8_t hats[] = {8, 0, 4, 8, 6, 7, 5, 6, 2, 1, 3, 2, 8, 0, 4, 8};
	struct ctrl_state input = {0};
	struct gamepad_report report;

	for (unsigned int i = 0; i < 16; i++) {
		input.buttons = ctrl_buttons[i];
		gamepad_from_ctrl(&input, &report);
		assert(report.buttons == (1u << i));
		assert(report.hat == 8);
	}
	for (unsigned int i = 0; i < 16; i++) {
		input.buttons = ((i & 1) ? BTN_DUP : 0) | ((i & 2) ? BTN_DDOWN : 0) |
				((i & 4) ? BTN_DLEFT : 0) | ((i & 8) ? BTN_DRIGHT : 0);
		gamepad_from_ctrl(&input, &report);
		assert(report.hat == hats[i]);
	}
	input = (struct ctrl_state) {
		.lx = INT16_MIN, .ly = INT16_MIN, .rx = INT16_MAX, .ry = INT16_MAX,
		.lt = 32767, .rt = UINT16_MAX,
	};
	gamepad_from_ctrl(&input, &report);
	assert(report.lx == -32767 && report.ly == 32767);
	assert(report.rx == 32767 && report.ry == -32767);
	assert(report.lt == 255 && report.rt == 255);
	input.lt = input.rt = 127;
	gamepad_from_ctrl(&input, &report);
	assert(report.lt == 0 && report.rt == 0);
	input.lt = input.rt = 128;
	gamepad_from_ctrl(&input, &report);
	assert(report.lt == 1 && report.rt == 1);
	input.lt = input.rt = 0;
	input.buttons = BTN_LT_CLICK | BTN_RT_CLICK;
	gamepad_from_ctrl(&input, &report);
	assert(report.lt == 255 && report.rt == 255);
}

static void test_wire_encoding(void)
{
	struct gamepad_report report;
	uint8_t generic[GAMEPAD_REPORT_SIZE], wired[SWITCH_REPORT_SIZE];
	const uint8_t generic_neutral[] = {0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	const uint8_t wired_neutral[] = {0, 0, 8, 128, 128, 128, 128, 0};
	const uint8_t generic_golden[] = {
		0x5A, 0xA5, 7, 1, 0x80, 0xFF, 0x7F, 0xFF, 0xFF, 0x34, 0x12, 255, 128,
	};

	gamepad_neutral(&report);
	gamepad_encode_generic(&report, generic);
	switch_encode_report(&report, wired);
	assert(memcmp(generic, generic_neutral, sizeof(generic)) == 0);
	assert(memcmp(wired, wired_neutral, sizeof(wired)) == 0);
	report = (struct gamepad_report) {
		.buttons = 0xA55A, .hat = 7, .lx = -32767, .ly = 32767,
		.rx = -1, .ry = 0x1234, .lt = 255, .rt = 128,
	};
	gamepad_encode_generic(&report, generic);
	assert(memcmp(generic, generic_golden, sizeof(generic)) == 0);

	/* Explicit wire bits: Y B A X L R ZL ZR Minus Plus L3 R3 Home. */
	const unsigned int expected[] = {1, 2, 0, 3, 4, 5, 8, 9, 10, 11, 12};

	gamepad_neutral(&report);
	for (unsigned int i = 0; i < 16; i++) {
		report.buttons = (uint16_t)(1u << i);
		switch_encode_report(&report, wired);
		unsigned int buttons = wired[0] | ((unsigned int)wired[1] << 8);

		assert(buttons == (i < 11 ? (1u << expected[i]) : 0));
		assert(wired[7] == 0);
	}
	report.buttons = 0;
	report.lt = 127;
	report.rt = 128;
	switch_encode_report(&report, wired);
	assert(wired[0] == 0x80 && wired[1] == 0);
	report.lt = 128;
	report.rt = 127;
	switch_encode_report(&report, wired);
	assert(wired[0] == 0x40 && wired[1] == 0);

	for (int32_t axis = INT16_MIN; axis <= INT16_MAX; axis++) {
		report.lx = report.ly = report.rx = report.ry = (int16_t)axis;
		switch_encode_report(&report, wired);
		for (unsigned int i = 3; i <= 6; i++) {
			assert(wired[i] == (axis + 32768) / 256);
		}
	}
}

int main(void)
{
	test_descriptors();
	test_controller_mapping();
	test_wire_encoding();
	puts("USB descriptors, controller mapping, and wire encoding passed");
	return 0;
}
