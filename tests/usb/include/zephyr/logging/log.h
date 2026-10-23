/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

enum test_log_level { TEST_LOG_INFO, TEST_LOG_WARN, TEST_LOG_ERROR };
void test_log(enum test_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(name, level)
#define LOG_INF(...) test_log(TEST_LOG_INFO, __VA_ARGS__)
#define LOG_WRN(...) test_log(TEST_LOG_WARN, __VA_ARGS__)
#define LOG_ERR(...) test_log(TEST_LOG_ERROR, __VA_ARGS__)
