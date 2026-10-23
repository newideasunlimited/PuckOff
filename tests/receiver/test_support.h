/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
		abort(); \
	} \
} while (0)

extern uint64_t test_time_us;
extern void (*test_clock_hook)(void);
extern int test_thread_count;
extern int test_hwinfo_result;
extern int test_ready_logs;
extern bool test_led_ready;
extern int test_led_config_error;
extern int test_led_write_error;

void test_log(const char *format, ...);
