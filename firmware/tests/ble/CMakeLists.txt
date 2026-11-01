# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(puckoff_ble_tests C)
enable_testing()

function(ble_test name)
  add_executable(ble_${name}_test ${name}_test.c ${ARGN})
  target_include_directories(ble_${name}_test PRIVATE fakes)
  target_compile_features(ble_${name}_test PRIVATE c_std_11)
  target_compile_options(ble_${name}_test PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME ble_${name} COMMAND ble_${name}_test)
endfunction()

ble_test(report_state ../../src/ble/report_state.c)
ble_test(remote ../../src/ble/remote.c)
ble_test(hid_map ../../src/ble/hid_map.c ../../src/gamepad.c)
ble_test(modes ../../src/ble/modes.c)
