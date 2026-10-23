/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include "input.h"
#include "radio.h"
#include <string.h>

static void report(uint8_t *bytes, uint8_t id, uint8_t seq)
{
	memset(bytes, 0, 46);
	bytes[0] = id;
	bytes[1] = seq;
	bytes[2] = 0x01;
	bytes[5] = 0x20;
	bytes[6] = 0xFF; bytes[7] = 0x7F;
	bytes[8] = 0x34; bytes[9] = 0x12;
	bytes[10] = 0x00; bytes[11] = 0x80;
	bytes[12] = 0xFF; bytes[13] = 0x7F;
	bytes[14] = 0xFF; bytes[15] = 0xFF;
	bytes[28] = 0xCD; bytes[29] = 0xAB;
	bytes[30] = 0x78; bytes[31] = 0x56; bytes[32] = 0x34; bytes[33] = 0x12;
	bytes[34] = 0x00; bytes[35] = 0x80;
	bytes[44] = 0xFF; bytes[45] = 0x7F;
}

static void decoding(void)
{
	uint8_t bytes[46];
	struct ctrl_state state, before;

	memset(&state, 0xA5, sizeof(state));
	before = state;
	report(bytes, 0x42, 255);
	CHECK(!input_decode_report(NULL, 46, &state));
	CHECK(!input_decode_report(bytes, 46, NULL));
	for (size_t size = 0; size < 30; size++) {
		CHECK(!input_decode_report(bytes, size, &state));
		CHECK(memcmp(&state, &before, sizeof(state)) == 0);
	}
	CHECK(input_decode_report(bytes, 46, &state));
	CHECK(state.report_id == 0x42 && state.seq == 255);
	CHECK(state.buttons == (BTN_A | BTN_LGRIP_TOUCH));
	CHECK(state.lt == 32767 && state.rt == 0x1234);
	CHECK(state.lx == INT16_MIN && state.ly == INT16_MAX && state.rx == -1);
	CHECK(state.rpad_force == 0xABCD && state.has_imu);
	CHECK(state.imu_time_us == 0x12345678 && state.accel[0] == INT16_MIN);
	CHECK(state.gyro[2] == INT16_MAX);
	report(bytes, 0x45, 0);
	CHECK(input_decode_report(bytes, 30, &state));
	CHECK(state.report_id == 0x45 && state.seq == 0 && !state.has_imu);
	CHECK(state.imu_time_us == 0 && state.gyro[2] == 0);
}

static void framing(void)
{
	uint8_t packet[RADIO_PACKET_SIZE] = {0};
	uint8_t bytes[46];
	struct ctrl_state state, before;

	report(bytes, 0x42, 1);
	packet[0] = 33; packet[2] = 0xF1; packet[3] = 30; packet[4] = 6;
	memcpy(packet + 5, bytes, 30);
	CHECK(input_from_packet(packet, 35, &state));
	CHECK(state.seq == 1);
	before = state;
	for (size_t size = 0; size < 35; size++) {
		CHECK(!input_from_packet(packet, size, &state));
		CHECK(memcmp(&state, &before, sizeof(state)) == 0);
	}
	CHECK(!input_from_packet(NULL, 35, &state));
	CHECK(!input_from_packet(packet, 35, NULL));
	packet[2] = 0xF3;
	CHECK(!input_from_packet(packet, sizeof(packet), &state));
	packet[2] = 0xF1;
	packet[0] = 255;
	CHECK(!input_from_packet(packet, sizeof(packet), &state));
	packet[0] = 34;
	CHECK(!input_from_packet(packet, sizeof(packet), &state)); /* trailing header */
	packet[0] = 35; packet[35] = 0; packet[36] = 6;
	CHECK(!input_from_packet(packet, sizeof(packet), &state)); /* empty record */
	packet[35] = 30;
	CHECK(!input_from_packet(packet, sizeof(packet), &state)); /* truncated value */
	CHECK(memcmp(&state, &before, sizeof(state)) == 0);

	packet[0] = 65; packet[35] = 30; packet[36] = 6;
	report(bytes, 0x45, 0); /* Packet order, not the wrapping sequence, wins. */
	memcpy(packet + 37, bytes, 30);
	CHECK(input_from_packet(packet, 67, &state));
	CHECK(state.seq == 0 && state.report_id == 0x45);
	before = state;
	packet[35] = 1; packet[0] = 36;
	CHECK(!input_from_packet(packet, 38, &state)); /* short recognized report */
	CHECK(memcmp(&state, &before, sizeof(state)) == 0);
	packet[37] = 0x99;
	CHECK(input_from_packet(packet, 38, &state)); /* unknown HID report is skipped */
	CHECK(state.seq == 1);
	packet[36] = 0x99;
	CHECK(input_from_packet(packet, 38, &state)); /* unknown record is skipped */
}

static void seeded_bounds(void)
{
	uint32_t random = 0x5EED1234;

	for (unsigned n = 0; n < 30000; n++) {
		size_t size = n % (RADIO_PACKET_SIZE + 1);
		uint8_t *packet = malloc(size ? size : 1);
		struct ctrl_state state, before;

		CHECK(packet != NULL);
		for (size_t i = 0; i < size; i++) {
			random = random * 1664525U + 1013904223U;
			packet[i] = (uint8_t)(random >> 24);
		}
		if (size >= 3 && (n & 1)) { packet[2] = 0xF1; }
		memset(&state, 0xA5, sizeof(state));
		before = state;
		if (!input_from_packet(packet, size, &state)) {
			CHECK(memcmp(&state, &before, sizeof(state)) == 0);
		} else {
			CHECK(size >= 35 && packet[0] <= RADIO_MAX_PAYLOAD);
			CHECK((size_t)packet[0] + 2 <= size);
			CHECK(state.report_id == 0x42 || state.report_id == 0x45);
		}
		free(packet);
	}
}

int main(void)
{
	decoding();
	framing();
	seeded_bounds();
	puts("input: reports, transactional framing, 30,000 seeded bounds cases");
	return 0;
}
