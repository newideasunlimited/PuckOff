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

add_executable(rf_window_test
	rf_window_test.c
	../../src/ble/rf_window.c
)
target_compile_features(rf_window_test PRIVATE c_std_11)
target_compile_options(rf_window_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME rf_window COMMAND rf_window_test)

add_executable(radio_mpsl_test
	radio_mpsl_test.c
	../../src/ble/rf_window.c
)
target_include_directories(radio_mpsl_test PRIVATE fakes)
target_compile_features(radio_mpsl_test PRIVATE c_std_11)
target_compile_options(radio_mpsl_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME radio_mpsl COMMAND radio_mpsl_test)

foreach(name link main)
	add_executable(${name}_ble_test
		${name}_ble_test.c
		../../../tests/receiver/fake_kernel.c
	)
	target_include_directories(${name}_ble_test PRIVATE ../../../tests/receiver/fakes)
	target_compile_definitions(${name}_ble_test PRIVATE
		CONFIG_PUCKOFF_TRANSPORT_BLE_RF=1 CONFIG_SYS_CLOCK_TICKS_PER_SEC=32768)
	target_compile_features(${name}_ble_test PRIVATE c_std_11)
	target_compile_options(${name}_ble_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
	add_test(NAME ${name}_ble COMMAND ${name}_ble_test)
endforeach()
target_sources(link_ble_test PRIVATE ../../src/input.c)
