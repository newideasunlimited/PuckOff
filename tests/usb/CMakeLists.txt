# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
project(puckoff_usb_tests C)

enable_testing()
set(ZEPHYR_BASE "$ENV{ZEPHYR_BASE}" CACHE PATH "Zephyr source tree (HID definitions only)")
if(NOT EXISTS "${ZEPHYR_BASE}/include/zephyr/usb/class/hid.h")
	message(FATAL_ERROR "Set ZEPHYR_BASE to the SDK's Zephyr source tree")
endif()

set(FIRMWARE "${CMAKE_CURRENT_SOURCE_DIR}/../../firmware/src")
add_executable(usb_reports test_reports.c "${FIRMWARE}/gamepad.c" "${FIRMWARE}/usb_switch.c")
add_test(NAME usb.reports COMMAND usb_reports)

foreach(mode generic switch switch_recovery)
	add_executable(usb_${mode} test_transport.c "${FIRMWARE}/usb.c"
		"${FIRMWARE}/gamepad.c" "${FIRMWARE}/usb_switch.c")
	if(NOT mode STREQUAL generic)
		target_compile_definitions(usb_${mode} PRIVATE CONFIG_PUCKOFF_USB_SWITCH=1)
	else()
		target_compile_definitions(usb_${mode} PRIVATE CONFIG_USBD_CDC_ACM_CLASS=1)
	endif()
	if(mode STREQUAL switch_recovery)
		target_compile_definitions(usb_${mode} PRIVATE CONFIG_PUCKOFF_USB_TEST_RECOVERY=1)
	endif()
	add_test(NAME usb.${mode} COMMAND usb_${mode})
endforeach()

foreach(target usb_reports usb_generic usb_switch usb_switch_recovery)
	target_compile_features(${target} PRIVATE c_std_11)
	target_compile_definitions(${target} PRIVATE CONFIG_ARCH_POSIX=1)
	target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG)
	target_include_directories(${target} PRIVATE include "${FIRMWARE}" "${ZEPHYR_BASE}/include")
endforeach()
