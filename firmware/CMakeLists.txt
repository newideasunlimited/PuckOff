# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(puckoff)

target_sources(app PRIVATE src/gamepad.c)

if(CONFIG_PUCKOFF_TRANSPORT_USB_RAW_RF)
	if(CONFIG_BT OR CONFIG_MPSL)
		message(FATAL_ERROR
			"The raw RF driver owns RADIO exclusively. Disable Bluetooth/MPSL "
			"or select an explicitly isolated BLE transport configuration.")
	endif()
	target_sources(app PRIVATE
		src/main.c
		src/radio.c
		src/link.c
		src/bond.c
		src/input.c
		src/usb.c
		src/console.c
	)
elseif(CONFIG_PUCKOFF_TRANSPORT_BLE_DIAGNOSTIC OR CONFIG_PUCKOFF_TRANSPORT_BLE_RF)
	if(NOT CONFIG_BT_LL_SOFTDEVICE)
		message(FATAL_ERROR "The BLE transport requires the Nordic SoftDevice Controller.")
	endif()
	target_sources(app PRIVATE
		src/ble/gamepad.c
		src/ble/report_state.c
		src/ble/console.c
	)
	if(CONFIG_PUCKOFF_TRANSPORT_BLE_DIAGNOSTIC)
		target_sources(app PRIVATE src/ble/diagnostic.c)
	else()
		target_sources(app PRIVATE
			src/main.c
			src/link.c
			src/bond.c
			src/input.c
			src/console.c
			src/ble/radio_mpsl.c
			src/ble/rf_window.c
		)
	endif()
else()
	message(FATAL_ERROR "Select exactly one PuckOff transport and radio owner.")
endif()
