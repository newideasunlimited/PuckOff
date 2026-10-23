/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Controller button bits, as sent by the Steam Controller. */
enum ctrl_button {
	BTN_A = 1u << 0,
	BTN_B = 1u << 1,
	BTN_X = 1u << 2,
	BTN_Y = 1u << 3,
	BTN_QAM = 1u << 4,
	BTN_R3 = 1u << 5,
	BTN_MENU = 1u << 6, /* right of the Steam button ("start") */
	BTN_R4 = 1u << 7,
	BTN_R5 = 1u << 8,
	BTN_RB = 1u << 9,
	BTN_DDOWN = 1u << 10,
	BTN_DRIGHT = 1u << 11,
	BTN_DLEFT = 1u << 12,
	BTN_DUP = 1u << 13,
	BTN_VIEW = 1u << 14, /* left of the Steam button ("select") */
	BTN_L3 = 1u << 15,
	BTN_STEAM = 1u << 16,
	BTN_L4 = 1u << 17,
	BTN_L5 = 1u << 18,
	BTN_LB = 1u << 19,
	BTN_RS_TOUCH = 1u << 20,
	BTN_RPAD_TOUCH = 1u << 21,
	BTN_RPAD_CLICK = 1u << 22,
	BTN_RT_CLICK = 1u << 23,
	BTN_LS_TOUCH = 1u << 24,
	BTN_LPAD_TOUCH = 1u << 25,
	BTN_LPAD_CLICK = 1u << 26,
	BTN_LT_CLICK = 1u << 27,
	BTN_RGRIP_TOUCH = 1u << 28,
	BTN_LGRIP_TOUCH = 1u << 29,
};

/* Decoded controller state. Stick Y is positive = up. Triggers 0..32767. */
struct ctrl_state {
	uint8_t report_id;
	uint8_t seq;
	uint32_t buttons;
	uint16_t lt, rt;
	int16_t lx, ly, rx, ry;
	int16_t lpad_x, lpad_y;
	uint16_t lpad_force;
	int16_t rpad_x, rpad_y;
	uint16_t rpad_force;
	bool has_imu;
	uint32_t imu_time_us;
	int16_t accel[3];
	int16_t gyro[3];
};

/* Decode one raw input report (starting at its report id byte). */
bool input_decode_report(const uint8_t *rep, size_t len, struct ctrl_state *out);

/*
 * Scan a received radio packet ([length][S1][type][records...]) for input
 * reports. Returns true if at least one was found; *out holds the newest.
 */
bool input_from_packet(const uint8_t *pkt, struct ctrl_state *out);
