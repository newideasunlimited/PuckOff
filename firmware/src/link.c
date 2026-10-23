/* SPDX-License-Identifier: Apache-2.0 */
/* One radio owner and one generic USB gamepad; see docs/protocol.md. */
#include "link.h"
#include "bond.h"
#include "radio.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/atomic.h>
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
#define POLL_PERIOD_US 4000U
#define POLL_REPLY_WINDOW_US 1200U
#define ADVERTISE_REPLY_WINDOW_US 800U
#define LINK_TIMEOUT_MS 300U
#define ADVERTISE_PERIOD_CONNECTED_MS 200U
#define ADVERTISE_PERIOD_IDLE_MS 20U
#define SESSION_BEACON_PERIOD_MS 25U
#define GOODBYE_BACKOFF_MS 2500U
#define POLL_BUDGET_US (RADIO_EXCHANGE_OVERHEAD_US + POLL_REPLY_WINDOW_US)

static const struct radio_addr rendezvous = {
	.base = { 'i', 'b', 'e', 'x' },
	.prefix = 0x10,
};

struct slot_state {
	struct bond bond;
	struct radio_addr session;
	uint8_t adv_pid;
	uint8_t poll_pid;
	uint32_t last_input_ms;
	uint32_t backoff_start_ms;
	uint32_t last_adv_ms;
	uint32_t last_beacon_ms;
	bool advertised;
	bool beaconed;
	bool backoff;
	bool up;
	struct link_stats stats;
};

enum poll_result {
	POLL_MISSED,
	POLL_HANDLED,
	POLL_ERROR,
};

static struct slot_state slots[BOND_SLOTS];
static uint8_t tx_buf[RADIO_PACKET_SIZE];
static uint8_t rx_buf[RADIO_PACKET_SIZE];
static uint32_t device_salt;
static uint32_t next_poll_us;
static int active_slot = -1;
static int next_slot;
static link_input_cb input_cb;
static link_lost_cb lost_cb;
static atomic_t verbose;
static atomic_t started;
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
	if (b == 0x00 || b == 0xFF || b == 0x55 || b == 0xAA) {
		b ^= 0x3C;
	}
	return b;
}

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

static bool same_bond(const struct bond *a, const struct bond *b)
{
	return a->used == b->used && a->receiver_id == b->receiver_id &&
	       a->controller_id == b->controller_id &&
	       memcmp(a->serial, b->serial, sizeof(a->serial)) == 0;
}

static void mark_lost(int slot)
{
	struct slot_state *s = &slots[slot];

	if (active_slot == slot) {
		active_slot = -1;
		next_slot = 0;
	}
	if (!s->up) {
		return;
	}
	s->up = false;
	K_SPINLOCK(&stats_lock) {
		s->stats.connected = false;
	}
	LOG_INF("slot %d: input released", slot);
	if (lost_cb) {
		lost_cb(slot);
	}
}

static void refresh_sessions(void)
{
	for (int i = 0; i < BOND_SLOTS; i++) {
		struct bond b = {0};
		int err = bond_snapshot(i, &b);

		if (err) {
			LOG_ERR("slot %d: bond snapshot failed (%d)", i, err);
		}
		if (same_bond(&slots[i].bond, &b)) {
			continue;
		}
		mark_lost(i);
		K_SPINLOCK(&stats_lock) {
			memset(&slots[i], 0, sizeof(slots[i]));
			slots[i].bond = b;
		}
		if (b.used) {
			derive_session(i, &b, &slots[i].session);
		}
	}
}

static void record_error(int slot, int err)
{
	K_SPINLOCK(&stats_lock) {
		if (err == RADIO_RX_BAD_CRC) {
			slots[slot].stats.crc_errors++;
		} else if (err == RADIO_RX_INVALID) {
			slots[slot].stats.invalid_packets++;
		} else if (err == -EAGAIN || err == -EBUSY) {
			slots[slot].stats.deferred++;
		} else {
			slots[slot].stats.radio_errors++;
		}
	}
	if (err != RADIO_RX_BAD_CRC && err != RADIO_RX_INVALID &&
	    err != -EAGAIN && err != -EBUSY) {
		LOG_ERR_RATELIMIT("slot %d: radio exchange failed (%d)", slot, err);
	}
}

static enum poll_result poll_once(int slot, uint32_t budget_us)
{
	struct slot_state *s = &slots[slot];
	struct ctrl_state st;
	struct bond current;
	int rx;

	tx_buf[0] = 1;
	tx_buf[1] = (uint8_t)(((s->poll_pid++ & 3) << 1) | 1);
	tx_buf[2] = PKT_POLL;
	rx = radio_exchange(SESSION_CHANNEL, &s->session, tx_buf, 3, rx_buf,
			    sizeof(rx_buf), POLL_REPLY_WINDOW_US, budget_us);
	K_SPINLOCK(&stats_lock) {
		s->stats.polls++;
	}
	if (rx < 0) {
		record_error(slot, rx);
		return rx == RADIO_RX_BAD_CRC || rx == RADIO_RX_INVALID ?
		       POLL_MISSED : POLL_ERROR;
	}
	if (rx != RADIO_RX_OK) {
		return POLL_MISSED;
	}
	if (rx_buf[0] < 1 || rx_buf[0] > RADIO_MAX_PAYLOAD ||
	    (rx_buf[2] != PKT_INPUT && rx_buf[2] != PKT_GOODBYE && rx_buf[2] != PKT_STATUS) ||
	    (rx_buf[2] == PKT_INPUT &&
	     !input_from_packet(rx_buf, (size_t)rx_buf[0] + 2, &st))) {
		record_error(slot, RADIO_RX_INVALID);
		return POLL_MISSED;
	}

	int err = bond_snapshot(slot, &current);

	if (err || !same_bond(&current, &s->bond)) {
		if (err) {
			LOG_ERR("slot %d: bond snapshot failed (%d)", slot, err);
		}
		refresh_sessions();
		return POLL_ERROR;
	}

	uint32_t now_ms = k_uptime_get_32();
	uint8_t type = rx_buf[2];

	K_SPINLOCK(&stats_lock) {
		s->stats.replies++;
		s->stats.rssi = radio_last_rssi();
		s->stats.last_reply_ms = now_ms;
		s->stats.powered_off = (type == PKT_GOODBYE);
	}
	if (type == PKT_GOODBYE) {
		mark_lost(slot);
		s->backoff = true;
		s->backoff_start_ms = now_ms;
		return POLL_HANDLED;
	}
	if (type == PKT_INPUT) {
		if (!s->up) {
			s->up = true;
			active_slot = slot;
			LOG_INF("slot %d: controller selected", slot);
		}
		s->last_input_ms = now_ms;
		K_SPINLOCK(&stats_lock) {
			s->stats.connected = true;
			s->stats.reports++;
		}
		if (input_cb) {
			input_cb(slot, &st);
		}
	}
	return POLL_HANDLED;
}

static bool available(int slot, uint32_t now_ms)
{
	struct slot_state *s = &slots[slot];

	if (s->backoff && now_ms - s->backoff_start_ms >= GOODBYE_BACKOFF_MS) {
		s->backoff = false;
	}
	return s->bond.used && !s->backoff;
}

static int choose_slot(uint32_t now_ms)
{
	if (active_slot >= 0) {
		return active_slot;
	}
	for (int n = 0; n < BOND_SLOTS; n++) {
		int slot = (next_slot + n) % BOND_SLOTS;

		if (available(slot, now_ms)) {
			next_slot = (slot + 1) % BOND_SLOTS;
			return slot;
		}
	}
	return -1;
}

static uint32_t frame_remaining(void)
{
	int32_t remaining = (int32_t)(next_poll_us - radio_now_us());

	return remaining > 0 ? (uint32_t)remaining : 0;
}

static void advertise(int slot, bool on_session, uint32_t budget_us)
{
	struct slot_state *s = &slots[slot];

	memset(tx_buf, 0, 20);
	tx_buf[0] = 18;
	tx_buf[1] = (uint8_t)((s->adv_pid++ & 3) << 1);
	tx_buf[2] = PKT_ADVERTISE;
	sys_put_le32(s->bond.receiver_id, &tx_buf[3]);
	sys_put_le32(s->bond.controller_id, &tx_buf[7]);
	tx_buf[11] = SESSION_CHANNEL;
	memcpy(&tx_buf[15], s->session.base, 4);
	tx_buf[19] = s->session.prefix;
	int result = radio_exchange(on_session ? SESSION_CHANNEL : RENDEZVOUS_CHANNEL,
				    on_session ? &s->session : &rendezvous, tx_buf, 20,
				    on_session ? NULL : rx_buf, on_session ? 0 : sizeof(rx_buf),
				    on_session ? 0 : ADVERTISE_REPLY_WINDOW_US, budget_us);

	if (result < 0) {
		record_error(slot, result);
		return;
	}
	if (on_session) {
		s->beaconed = true;
		s->last_beacon_ms = k_uptime_get_32();
	} else {
		s->advertised = true;
		s->last_adv_ms = k_uptime_get_32();
		if (result == RADIO_RX_OK && atomic_get(&verbose)) {
			LOG_INF("slot %d: advertise reply (len %u)", slot, rx_buf[0]);
		}
	}
}

static void link_step(void)
{
	refresh_sessions();
	uint32_t now_ms = k_uptime_get_32();
	uint32_t now_us = radio_now_us();

	if (active_slot >= 0 && now_ms - slots[active_slot].last_input_ms >= LINK_TIMEOUT_MS) {
		mark_lost(active_slot);
	}
	if ((int32_t)(now_us - next_poll_us) < 0) {
		return;
	}

	/* Skip missed frames rather than issuing a burst of catch-up polls. */
	next_poll_us += POLL_PERIOD_US;
	if ((int32_t)(now_us - next_poll_us) >= 0) {
		next_poll_us = now_us + POLL_PERIOD_US;
	}
	int slot = choose_slot(now_ms);
	uint32_t remaining = frame_remaining();

	if (slot < 0 || remaining < POLL_BUDGET_US) {
		return;
	}
	enum poll_result result = poll_once(slot, remaining);

	remaining = frame_remaining();
	if (result == POLL_MISSED && active_slot == slot && remaining >= POLL_BUDGET_US &&
	    k_uptime_get_32() - slots[slot].last_input_ms < LINK_TIMEOUT_MS) {
		(void)poll_once(slot, remaining);
	}

	now_ms = k_uptime_get_32();
	remaining = frame_remaining();
	if (!available(slot, now_ms)) {
		return;
	}
	struct slot_state *s = &slots[slot];
	uint32_t adv_period = active_slot >= 0 ?
			      ADVERTISE_PERIOD_CONNECTED_MS : ADVERTISE_PERIOD_IDLE_MS;

	/* At most one maintenance exchange, and only inside this frame's budget. */
	if ((!s->beaconed || now_ms - s->last_beacon_ms >= SESSION_BEACON_PERIOD_MS) &&
	    remaining >= RADIO_EXCHANGE_OVERHEAD_US) {
		advertise(slot, true, remaining);
	} else if ((!s->advertised || now_ms - s->last_adv_ms >= adv_period) &&
		   remaining >= RADIO_EXCHANGE_OVERHEAD_US + ADVERTISE_REPLY_WINDOW_US) {
		advertise(slot, false, remaining);
	}
}

static void link_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		link_step();
		k_usleep(MIN(frame_remaining(), 1000U));
	}
}

K_THREAD_STACK_DEFINE(link_stack, 2048);
static struct k_thread link_tid;

int link_start(link_input_cb on_input, link_lost_cb on_lost)
{
	uint8_t id[8];
	int err;

	if (!atomic_cas(&started, 0, 1)) {
		return -EALREADY;
	}
	err = hwinfo_get_device_id(id, sizeof(id));
	if (err != (int)sizeof(id)) {
		atomic_clear(&started);
		return err < 0 ? err : -EIO;
	}
	err = radio_init();
	if (err) {
		atomic_clear(&started);
		return err;
	}
	device_salt = mix32(sys_get_le32(id) ^ mix32(sys_get_le32(id + 4)));
	input_cb = on_input;
	lost_cb = on_lost;
	next_poll_us = radio_now_us();
	k_thread_create(&link_tid, link_stack, K_THREAD_STACK_SIZEOF(link_stack), link_thread,
			NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	err = k_thread_name_set(&link_tid, "radio");
	if (err) {
		LOG_WRN("radio thread name failed (%d)", err);
	}
	LOG_INF("radio ready");
	return 0;
}

int link_get_stats(int slot, struct link_stats *out)
{
	if (slot < 0 || slot >= BOND_SLOTS || out == NULL) {
		return -EINVAL;
	}
	K_SPINLOCK(&stats_lock) {
		*out = slots[slot].stats;
	}
	return 0;
}

bool link_any_connected(void)
{
	bool connected = false;

	K_SPINLOCK(&stats_lock) {
		for (int i = 0; i < BOND_SLOTS; i++) {
			connected |= slots[i].stats.connected;
		}
	}
	return connected;
}

void link_set_verbose(bool on)
{
	atomic_set(&verbose, on);
}
