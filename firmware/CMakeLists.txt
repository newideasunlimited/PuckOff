# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(puckoff)

target_sources(app PRIVATE src/gamepad.c)

if(CONFIG_PUCKOFF_TRANSPORT_USB_RAW_RF)
	if(CONFIG_BT OR CONFIG_MPSL)
		message(FATAL_ERROR
			"The raw RF driver owns RADIO exclusively. Disable Bluetooth/MPSL "
			"or select the isolated BLE diagnostic configuration.")
	endif()
	target_sources(app PRIVATE
		src/main.c
		src/radio.c
		src/link.c
		src/bond.c
		src/input.c
		src/usb.c
	)
	target_sources_ifdef(CONFIG_PUCKOFF_USB_SWITCH app PRIVATE src/usb_switch.c)
	target_sources_ifdef(CONFIG_PUCKOFF_SNIFFER app PRIVATE src/sniff.c)
	target_sources_ifdef(CONFIG_SHELL app PRIVATE src/console.c)
elseif(CONFIG_PUCKOFF_TRANSPORT_BLE_DIAGNOSTIC)
	if(NOT CONFIG_BT_LL_SOFTDEVICE)
		message(FATAL_ERROR "The BLE diagnostic requires the Nordic SoftDevice Controller.")
	endif()
	target_sources(app PRIVATE
		src/ble/diagnostic.c
		src/ble/gamepad.c
		src/ble/report_state.c
	)
elseif(CONFIG_PUCKOFF_TRANSPORT_BLE_BRIDGE)
	if(CONFIG_MPSL_TIMESLOT_SESSION_COUNT LESS 1)
		message(FATAL_ERROR "The BLE bridge needs one MPSL timeslot session.")
	endif()
	target_sources(app PRIVATE
		src/main.c
		src/radio_timeslot.c
		src/link.c
		src/bond.c
		src/input.c
		src/usb.c
		src/console.c
		src/ble/bridge.c
		src/ble/bridge_shell.c
		src/ble/gamepad.c
		src/ble/hid_map.c
		src/ble/modes.c
		src/ble/remote.c
		src/ble/report_state.c
	)
else()
	message(FATAL_ERROR "Select exactly one PuckOff transport and radio owner.")
endif()

target_sources_ifdef(CONFIG_PUCKOFF_BOOT_GUARD app PRIVATE src/boot_guard.c)
