/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Receiver side of the Steam Controller wireless link.
 *
 * For every paired slot we:
 *   - advertise ourselves on the shared rendezvous address so a controller
 *     that is looking for its receiver can find us and learn a private
 *     session address and channel;
 *   - poll the controller on that session address at 250 Hz and decode the
 *     input it sends back.
 *
 * See docs/protocol.md for the packet formats.
 */
#include "link.h"
#include "bond.h"
#include "radio.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(link, LOG_LEVEL_INF);

#define RENDEZVOUS_CHANNEL 2
#define SESSION_CHANNEL 18

#define PKT_ADVERTISE 0xE1
#define PKT_POLL 0xE3
#define PKT_INPUT 0xF1
#define PKT_GOODBYE 0xF2
#define PKT_STATUS 0xF3

#define POLL_PERIOD_US 4000
#define POLL_REPLY_WINDOW_US 1200
#define ADVERTISE_REPLY_WINDOW_US 800
#define LINK_TIMEOUT_MS 300
#define ADVERTISE_PERIOD_CONNECTED_MS 200
#define SESSION_BEACON_PERIOD_MS 25
#define GOODBYE_BACKOFF_MS 2500
#define RADIO_STALL_MS 2500
#define IDLE_SLOT_RETRY_MS 100

static const struct radio_addr rendezvous = {
	.base = { 'i', 'b', 'e', 'x' },
	.prefix = 0x10,
};

struct slot_state {
	struct radio_addr session;
	uint32_t controller_id; /* bond the session address was derived for */
	uint8_t adv_pid;
	uint8_t poll_pid;
	uint32_t last_reply_ms;
	uint32_t last_poll_ms;
	uint32_t backoff_until_ms;
	bool up;
	struct link_stats stats;
};

static struct slot_state slots[BOND_SLOTS];
static uint8_t tx_buf[RADIO_MAX_PAYLOAD + 2] __aligned(4);
static uint8_t rx_buf[RADIO_MAX_PAYLOAD + 2] __aligned(4);
static uint32_t device_salt;
static link_input_cb input_cb;
static link_lost_cb lost_cb;
static bool verbose;
static struct k_spinlock stats_lock;

static uint32_t mix32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7FEB352DU;
	x ^= x >> 15;
	x *= 0x846CA68BU;
	x ^= x >> 16;
	return x;
}

static uint8_t usable_addr_byte(uint8_t b)
{
	/* Avoid bytes that look like preamble or carry no bit transitions. */
	if (b == 0x00 || b == 0xFF || b == 0x55 || b == 0xAA) {
		b ^= 0x3C;
	}
	return b;
}

/*
 * Each receiver picks its own private session address per controller so that
 * several receivers and controllers can share a room.
 */
static void derive_session(int slot, const struct bond *b, struct radio_addr *out)
{
	uint32_t h = mix32(b->receiver_id ^ mix32(b->controller_id ^ device_salt) ^
			   (uint32_t)slot * 0x9E3779B9U);
	uint32_t p = mix32(h ^ 0xA5C3F00DU);

	sys_put_be32(h, out->base);
	for (int i = 0; i < 4; i++) {
		out->base[i] = usable_addr_byte(out->base[i]);
	}
	out->prefix = usable_addr_byte((uint8_t)p);
	if (memcmp(out->base, rendezvous.base, 4) == 0) {
		out->base[0] ^= 0x01;
	}
}

static void build_advertise(int slot, const struct bond *b)
{
	struct slot_state *s = &slots[slot];

	memset(tx_buf, 0, sizeof(tx_buf));
	tx_buf[0] = 18;
	tx_buf[1] = (uint8_t)((s->adv_pid++ & 3) << 1);
	tx_buf[2] = PKT_ADVERTISE;
	sys_put_le32(b->receiver_id, &tx_buf[3]);
	sys_put_le32(b->controller_id, &tx_buf[7]);
	tx_buf[11] = SESSION_CHANNEL;
	memcpy(&tx_buf[15], s->session.base, 4);
	tx_buf[19] = s->session.prefix;
}

static void advertise(int slot, const struct bond *b, bool on_session)
{
	struct slot_state *s = &slots[slot];

	build_advertise(slot, b);
	if (on_session) {
		radio_send(SESSION_CHANNEL, &s->session, tx_buf);
		return;
	}
	radio_send(RENDEZVOUS_CHANNEL, &rendezvous, tx_buf);
	if (radio_listen(rx_buf, ADVERTISE_REPLY_WINDOW_US) == RADIO_RX_OK && verbose) {
		LOG_INF("slot %d: controller answered advertise (len %u)", slot, rx_buf[0]);
	}
}

static void mark_lost(int slot)
{
	struct slot_state *s = &slots[slot];

	if (!s->up) {
		return;
	}
	s->up = false;
	LOG_INF("slot %d: controller disconnected", slot);
	if (lost_cb) {
		lost_cb(slot);
	}
}

static bool poll_once(int slot, uint32_t now_ms)
{
	struct slot_state *s = &slots[slot];
	enum radio_rx_result rx;
	struct ctrl_state st;
	uint8_t type;

	tx_buf[0] = 1;
	tx_buf[1] = (uint8_t)(((s->poll_pid++ & 3) << 1) | 1);
	tx_buf[2] = PKT_POLL;
	radio_send(SESSION_CHANNEL, &s->session, tx_buf);
	rx = radio_listen(rx_buf, POLL_REPLY_WINDOW_US);

	K_SPINLOCK(&stats_lock) {
		s->stats.polls++;
		if (rx == RADIO_RX_BAD_CRC) {
			s->stats.crc_errors++;
		}
	}
	if (rx != RADIO_RX_OK || rx_buf[0] == 0) {
		return false;
	}

	type = rx_buf[2];
	if ((type & 0xF0) != 0xF0) {
		return false;
	}

	s->last_reply_ms = now_ms;
	K_SPINLOCK(&stats_lock) {
		s->stats.replies++;
		s->stats.rssi = radio_last_rssi();
		s->stats.last_reply_ms = now_ms;
		s->stats.powered_off = (type == PKT_GOODBYE);
	}
	if (!s->up && type != PKT_GOODBYE) {
		s->up = true;
		LOG_INF("slot %d: controller connected", slot);
	}

	if (type == PKT_GOODBYE) {
		LOG_INF("slot %d: controller turned off", slot);
		mark_lost(slot);
		s->backoff_until_ms = now_ms + GOODBYE_BACKOFF_MS;
		return true;
	}
	if (type == PKT_INPUT && input_from_packet(rx_buf, &st)) {
		K_SPINLOCK(&stats_lock) {
			s->stats.reports++;
		}
		if (input_cb) {
			input_cb(slot, &st);
		}
	}
	return true;
}

static void refresh_sessions(void)
{
	for (int i = 0; i < BOND_SLOTS; i++) {
		struct bond b;

		bond_snapshot(i, &b);
		if (b.used && slots[i].controller_id != b.controller_id) {
			derive_session(i, &b, &slots[i].session);
			slots[i].controller_id = b.controller_id;
		}
	}
}

static void link_thread(void *a, void *b, void *c)
{
	uint32_t next_poll_us = radio_now_us();
	uint32_t last_adv_ms = 0, last_beacon_ms = 0, last_rx_any_ms = 0;
	bool was_up = false;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (radio_init()) {
		LOG_ERR("radio init failed");
		return;
	}
	LOG_INF("radio ready");

	for (;;) {
		uint32_t now_ms = k_uptime_get_32();
		bool any_up = false;

		refresh_sessions();

		/* Link-loss detection. */
		for (int i = 0; i < BOND_SLOTS; i++) {
			if (slots[i].up && now_ms - slots[i].last_reply_ms > LINK_TIMEOUT_MS) {
				mark_lost(i);
			}
			any_up |= slots[i].up;
			K_SPINLOCK(&stats_lock) {
				slots[i].stats.connected = slots[i].up;
			}
		}

		/* 250 Hz input poll. */
		if ((int32_t)(radio_now_us() - next_poll_us) >= 0) {
			next_poll_us += POLL_PERIOD_US;
			if ((int32_t)(radio_now_us() - next_poll_us) >= 0) {
				next_poll_us = radio_now_us() + POLL_PERIOD_US;
			}
			for (int i = 0; i < BOND_SLOTS; i++) {
				struct bond bd;
				struct slot_state *s = &slots[i];

				bond_snapshot(i, &bd);
				if (!bd.used || (int32_t)(now_ms - s->backoff_until_ms) < 0) {
					continue;
				}
				/* Don't spend airtime polling an absent controller at full rate. */
				if (!s->up && any_up && now_ms - s->last_poll_ms < IDLE_SLOT_RETRY_MS) {
					continue;
				}
				s->last_poll_ms = now_ms;
				if (poll_once(i, now_ms)) {
					last_rx_any_ms = now_ms;
				} else if (s->up && now_ms - s->last_reply_ms < LINK_TIMEOUT_MS) {
					/* One quick retry while the link is healthy. */
					if (poll_once(i, now_ms)) {
						last_rx_any_ms = now_ms;
					}
				}
			}
		}

		/* Tell controllers where to find us. */
		if (!any_up || now_ms - last_beacon_ms >= SESSION_BEACON_PERIOD_MS) {
			last_beacon_ms = now_ms;
			for (int i = 0; i < BOND_SLOTS; i++) {
				struct bond bd;

				bond_snapshot(i, &bd);
				if (bd.used && (int32_t)(now_ms - slots[i].backoff_until_ms) >= 0) {
					advertise(i, &bd, true);
				}
			}
		}
		if (!any_up || now_ms - last_adv_ms >= ADVERTISE_PERIOD_CONNECTED_MS) {
			last_adv_ms = now_ms;
			for (int i = 0; i < BOND_SLOTS; i++) {
				struct bond bd;

				bond_snapshot(i, &bd);
				if (bd.used && (int32_t)(now_ms - slots[i].backoff_until_ms) >= 0) {
					advertise(i, &bd, false);
				}
			}
		}

		/* If a live link goes completely silent, power-cycle the radio. */
		if (any_up) {
			was_up = true;
		}
		if (was_up && !any_up && now_ms - last_rx_any_ms > RADIO_STALL_MS) {
			LOG_WRN("radio silent, resetting");
			radio_reset();
			was_up = false;
		}

		k_usleep(any_up ? 500 : 1000);
	}
}

K_THREAD_STACK_DEFINE(link_stack, 2048);
static struct k_thread link_tid;

void link_start(link_input_cb on_input, link_lost_cb on_lost)
{
	uint8_t id[8] = { 0 };

	input_cb = on_input;
	lost_cb = on_lost;
	(void)hwinfo_get_device_id(id, sizeof(id));
	device_salt = mix32(sys_get_le32(id) ^ mix32(sys_get_le32(id + 4)));

	k_thread_create(&link_tid, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_thread_name_set(&link_tid, "radio");
}

void link_get_stats(int slot, struct link_stats *out)
{
	K_SPINLOCK(&stats_lock) {
		*out = slots[slot].stats;
	}
}

bool link_any_connected(void)
{
	for (int i = 0; i < BOND_SLOTS; i++) {
		if (slots[i].up) {
			return true;
		}
	}
	return false;
}

void link_set_verbose(bool on)
{
	verbose = on;
}
