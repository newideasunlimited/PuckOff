/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Low-level driver for the nRF52840 RADIO peripheral, configured to speak the
 * Steam Controller's proprietary 2.4 GHz link. See docs/protocol.md.
 */
#include "radio.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <hal/nrf_radio.h>
#include <nrfx.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_BT) || defined(CONFIG_MPSL)
#error "The raw RADIO driver requires exclusive ownership; use a separate timeslot backend"
#endif
#if CONFIG_SYS_CLOCK_TICKS_PER_SEC < 32768
#error "The raw RADIO deadlines require the nRF RTC tick rate (32768 Hz)"
#endif

LOG_MODULE_REGISTER(radio, LOG_LEVEL_INF);

#define DISABLE_TIMEOUT_US 200U
#define TX_TIMEOUT_US 600U
#define HF_CLOCK_TIMEOUT_MS 100U

static uint8_t rx_scratch[RADIO_PACKET_SIZE] __aligned(4);
static uint8_t tx_scratch[RADIO_PACKET_SIZE] __aligned(4);
static uint8_t last_rssi;
static bool initialized;

uint32_t radio_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64(k_uptime_ticks());
}

static int wait_event(volatile uint32_t *event, uint32_t timeout_us)
{
	uint32_t start = radio_now_us();

	while (!*event) {
		if (radio_now_us() - start >= timeout_us) {
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static uint8_t bitrev8(uint8_t x)
{
	x = (uint8_t)((x & 0xF0) >> 4 | (x & 0x0F) << 4);
	x = (uint8_t)((x & 0xCC) >> 2 | (x & 0x33) << 2);
	x = (uint8_t)((x & 0xAA) >> 1 | (x & 0x55) << 1);
	return x;
}

static int radio_disable(void)
{
	NRF_RADIO->SHORTS = 0;
	if (NRF_RADIO->STATE == RADIO_STATE_STATE_Disabled) {
		return 0;
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_DISABLE = 1;
	return wait_event(&NRF_RADIO->EVENTS_DISABLED, DISABLE_TIMEOUT_US);
}

static int configure(uint8_t channel, const struct radio_addr *addr)
{
	int err = radio_disable();

	if (err) {
		return err;
	}

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
	return 0;
}

int radio_init(void)
{
	struct onoff_manager *mgr =
		z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	struct onoff_client cli = {0};
	uint32_t start = k_uptime_get_32();
	int res = 0;
	int err;

	if (initialized) {
		return 0;
	}
	if (mgr == NULL) {
		return -ENODEV;
	}
	/* The radio needs the crystal oscillator, not the internal RC. */
	sys_notify_init_spinwait(&cli.notify);
	err = onoff_request(mgr, &cli);
	if (err < 0) {
		return err;
	}
	while ((err = sys_notify_fetch_result(&cli.notify, &res)) == -EAGAIN &&
	       k_uptime_get_32() - start < HF_CLOCK_TIMEOUT_MS) {
		k_msleep(1);
	}
	if (err) {
		int cleanup = onoff_cancel_or_release(mgr, &cli);

		if (cleanup < 0) {
			LOG_ERR("HF clock request cleanup failed (%d)", cleanup);
		}
		return err == -EAGAIN ? -ETIMEDOUT : err;
	}
	if (res < 0) {
		return res;
	}
	radio_reset();
	initialized = true;
	return 0;
}

void radio_reset(void)
{
	NRF_RADIO->POWER = 0;
	(void)NRF_RADIO->POWER;
	NRF_RADIO->POWER = 1;
	NRF_RADIO->INTENCLR = 0xFFFFFFFF;
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->PACKETPTR = 0;
	last_rssi = 0;
}

static int radio_send(void)
{
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)tx_scratch;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_TXEN = 1;
	int err = wait_event(&NRF_RADIO->EVENTS_DISABLED, TX_TIMEOUT_US);

	if (err) {
		return err;
	}
	return NRF_RADIO->EVENTS_END ? 0 : -EIO;
}

static int radio_listen(uint8_t *pkt, size_t capacity, uint32_t timeout_us)
{
	int err;
	bool received;

	rx_scratch[0] = 0;
	rx_scratch[1] = 0;
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)rx_scratch;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_DISABLE_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk |
			    RADIO_SHORTS_DISABLED_RSSISTOP_Msk;
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_RXEN = 1;

	(void)wait_event(&NRF_RADIO->EVENTS_END, timeout_us);
	received = NRF_RADIO->EVENTS_END != 0;
	err = radio_disable();
	if (err) {
		return err;
	}
	if (!received) {
		return RADIO_RX_NONE;
	}
	if (!(NRF_RADIO->CRCSTATUS & 1)) {
		return RADIO_RX_BAD_CRC;
	}
	if (rx_scratch[0] < 1 || rx_scratch[0] > RADIO_MAX_PAYLOAD ||
	    (size_t)rx_scratch[0] + 2 > capacity) {
		return RADIO_RX_INVALID;
	}
	last_rssi = (uint8_t)(NRF_RADIO->RSSISAMPLE & 0x7F);
	memcpy(pkt, rx_scratch, (size_t)rx_scratch[0] + 2);
	return RADIO_RX_OK;
}

int radio_exchange(uint8_t channel, const struct radio_addr *addr,
		   const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_capacity,
		   uint32_t rx_timeout_us, uint32_t budget_us)
{
	if (addr == NULL || tx == NULL || tx_len < 3 || tx_len > RADIO_PACKET_SIZE ||
	    tx[0] < 1 || tx[0] > RADIO_MAX_PAYLOAD || (size_t)tx[0] + 2 != tx_len ||
	    tx[1] > 7 || channel > 100 || rx_timeout_us > RADIO_MAX_RX_TIMEOUT_US ||
	    budget_us > RADIO_MAX_EXCHANGE_US ||
	    (rx_timeout_us && (rx == NULL || rx_capacity < 3)) ||
	    (!rx_timeout_us && (rx != NULL || rx_capacity != 0))) {
		return -EINVAL;
	}
	if (!initialized) {
		return -ENODEV;
	}
	if (budget_us < RADIO_EXCHANGE_OVERHEAD_US + rx_timeout_us) {
		return -EAGAIN;
	}

	uint32_t start = radio_now_us();
	int err = configure(channel, addr);

	if (!err && radio_now_us() - start <=
		    budget_us - TX_TIMEOUT_US - rx_timeout_us - DISABLE_TIMEOUT_US) {
		memcpy(tx_scratch, tx, tx_len);
		err = radio_send();
	} else if (!err) {
		err = -EAGAIN;
	}
	if (!err && rx_timeout_us) {
		if (radio_now_us() - start >
		    budget_us - rx_timeout_us - DISABLE_TIMEOUT_US) {
			err = -EAGAIN;
		} else {
			err = radio_listen(rx, rx_capacity, rx_timeout_us);
		}
	}
	NRF_RADIO->SHORTS = 0;
	if (err == -ETIMEDOUT || err == -EIO) {
		radio_reset();
	}
	return err;
}

uint8_t radio_last_rssi(void)
{
	return last_rssi;
}
