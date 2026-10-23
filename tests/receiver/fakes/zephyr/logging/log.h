/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
void test_log(const char *format, ...);
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(name, level) typedef int log_module_##name
#define LOG_INF(...) test_log(__VA_ARGS__)
#define LOG_WRN(...) test_log(__VA_ARGS__)
#define LOG_ERR(...) test_log(__VA_ARGS__)
#define LOG_ERR_RATELIMIT(...) test_log(__VA_ARGS__)
