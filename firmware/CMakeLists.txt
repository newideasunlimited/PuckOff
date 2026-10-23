# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(puckoff)

target_sources(app PRIVATE
	src/main.c
	src/radio.c
	src/link.c
	src/bond.c
	src/input.c
	src/usb.c
	src/gamepad.c
)
target_sources_ifdef(CONFIG_PUCKOFF_USB_SWITCH app PRIVATE src/usb_switch.c)
target_sources_ifdef(CONFIG_SHELL app PRIVATE src/console.c)
