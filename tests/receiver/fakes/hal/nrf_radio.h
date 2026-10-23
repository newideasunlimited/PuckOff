/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

typedef struct {
	volatile uint32_t POWER, INTENCLR, SHORTS, STATE;
	volatile uint32_t EVENTS_DISABLED, EVENTS_END;
	volatile uint32_t TASKS_DISABLE, TASKS_TXEN, TASKS_RXEN;
	volatile uint32_t MODE, MODECNF0, TXPOWER, FREQUENCY, PCNF0, PCNF1;
	volatile uint32_t CRCCNF, CRCPOLY, CRCINIT, BASE0, PREFIX0;
	volatile uint32_t TXADDRESS, RXADDRESSES, PACKETPTR, CRCSTATUS, RSSISAMPLE;
} NRF_RADIO_Type;
extern NRF_RADIO_Type test_radio;
#define NRF_RADIO (&test_radio)
#define RADIO_STATE_STATE_Disabled 0
#define RADIO_MODE_MODE_Ble_2Mbit 4
#define RADIO_MODE_MODE_Pos 0
#define RADIO_MODECNF0_RU_Fast 1
#define RADIO_MODECNF0_RU_Pos 0
#define RADIO_TXPOWER_TXPOWER_Pos8dBm 8
#define RADIO_TXPOWER_TXPOWER_Pos 0
#define RADIO_PCNF0_LFLEN_Pos 0
#define RADIO_PCNF0_S1LEN_Pos 16
#define RADIO_PCNF1_ENDIAN_Big 1
#define RADIO_PCNF1_ENDIAN_Pos 24
#define RADIO_PCNF1_BALEN_Pos 16
#define RADIO_PCNF1_MAXLEN_Pos 0
#define RADIO_CRCCNF_LEN_Two 2
#define RADIO_CRCCNF_LEN_Pos 0
#define RADIO_SHORTS_READY_START_Msk (1U << 0)
#define RADIO_SHORTS_END_DISABLE_Msk (1U << 1)
#define RADIO_SHORTS_ADDRESS_RSSISTART_Msk (1U << 4)
#define RADIO_SHORTS_DISABLED_RSSISTOP_Msk (1U << 8)
