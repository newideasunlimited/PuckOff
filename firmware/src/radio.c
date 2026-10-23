/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Low-level driver for the nRF52840 RADIO peripheral, configured to speak the
 * Steam Controller's proprietary 2.4 GHz link. See docs/protocol.md.
 */
#include "radio.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_radio.h>
#include <nrfx.h>

#define DISABLE_TIMEOUT_US 3000

static uint8_t rx_scratch[RADIO_MAX_PAYLOAD + 2];
static uint8_t last_rssi;

uint32_t radio_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static uint32_t elapsed_us(uint32_t start_cyc)
{
	return (DWT->CYCCNT - start_cyc) / (SystemCoreClock / 1000000U);
}

static uint8_t bitrev8(uint8_t x)
{
	x = (uint8_t)((x & 0xF0) >> 4 | (x & 0x0F) << 4);
	x = (uint8_t)((x & 0xCC) >> 2 | (x & 0x33) << 2);
	x = (uint8_t)((x & 0xAA) >> 1 | (x & 0x55) << 1);
	return x;
}

static void wait_disabled(void)
{
	uint32_t t0 = DWT->CYCCNT;

	while (!NRF_RADIO->EVENTS_DISABLED && elapsed_us(t0) < DISABLE_TIMEOUT_US) {
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
}

static void radio_disable(void)
{
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	wait_disabled();
}

static void configure(uint8_t channel, const struct radio_addr *addr)
{
	radio_disable();

	NRF_RADIO->MODE = RADIO_MODE_MODE_Ble_2Mbit << RADIO_MODE_MODE_Pos;
	NRF_RADIO->MODECNF0 = RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos;
	NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->FREQUENCY = channel;

	/* 8-bit length field, 3-bit S1 field (packet id + no-ack flag). */
	NRF_RADIO->PCNF0 = (8U << RADIO_PCNF0_LFLEN_Pos) | (3U << RADIO_PCNF0_S1LEN_Pos);
	/* Big-endian, 4 base bytes + prefix, no whitening. */
	NRF_RADIO->PCNF1 = (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
			   (4U << RADIO_PCNF1_BALEN_Pos) |
			   (RADIO_MAX_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos);

	/* CRC-16-CCITT over address and payload. */
	NRF_RADIO->CRCCNF = RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos;
	NRF_RADIO->CRCPOLY = 0x11021;
	NRF_RADIO->CRCINIT = 0xFFFF;

	/* The air format sends each address byte bit-reversed. */
	NRF_RADIO->BASE0 = (uint32_t)bitrev8(addr->base[0]) << 24 |
			   (uint32_t)bitrev8(addr->base[1]) << 16 |
			   (uint32_t)bitrev8(addr->base[2]) << 8 |
			   (uint32_t)bitrev8(addr->base[3]);
	NRF_RADIO->PREFIX0 = bitrev8(addr->prefix);
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->RXADDRESSES = 1;
}

int radio_init(void)
{
	struct onoff_manager *mgr =
		z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	struct onoff_client cli;
	int res = 0;

	/* The radio needs the crystal oscillator, not the internal RC. */
	sys_notify_init_spinwait(&cli.notify);
	if (onoff_request(mgr, &cli) < 0) {
		return -EIO;
	}
	while (sys_notify_fetch_result(&cli.notify, &res) == -EAGAIN) {
	}

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	radio_reset();
	return res;
}

void radio_reset(void)
{
	NRF_RADIO->POWER = 0;
	NRF_RADIO->POWER = 1;
	NRF_RADIO->INTENCLR = 0xFFFFFFFF;
}

void radio_send(uint8_t channel, const struct radio_addr *addr, const uint8_t *pkt)
{
	configure(channel, addr);
	NRF_RADIO->PACKETPTR = (uint32_t)pkt;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_TXEN = 1;
	wait_disabled();
}

enum radio_rx_result radio_listen(uint8_t *pkt, uint32_t timeout_us)
{
	enum radio_rx_result result = RADIO_RX_NONE;
	uint8_t *buf = pkt ? pkt : rx_scratch;
	uint32_t t0;

	buf[0] = 0;
	NRF_RADIO->PACKETPTR = (uint32_t)buf;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk |
			    RADIO_SHORTS_DISABLED_RSSISTOP_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->TASKS_RXEN = 1;

	t0 = DWT->CYCCNT;
	while (!NRF_RADIO->EVENTS_END && elapsed_us(t0) < timeout_us) {
	}

	if (NRF_RADIO->EVENTS_END) {
		NRF_RADIO->EVENTS_END = 0;
		result = (NRF_RADIO->CRCSTATUS & 1) ? RADIO_RX_OK : RADIO_RX_BAD_CRC;
		last_rssi = (uint8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
	}
	radio_disable();
	return result;
}

uint8_t radio_last_rssi(void)
{
	return last_rssi;
}
