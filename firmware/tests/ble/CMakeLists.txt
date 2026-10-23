# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(puckoff_ble_tests C)
enable_testing()

add_executable(ble_report_state_test
	report_state_test.c
	../../src/ble/report_state.c
)
target_compile_features(ble_report_state_test PRIVATE c_std_11)
target_compile_options(ble_report_state_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME ble_report_state COMMAND ble_report_state_test)
