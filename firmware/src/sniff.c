/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Passive link sniffer. It never transmits: it waits on the rendezvous
 * address for another receiver's advertisement, follows it to the announced
 * session channel and address, and prints what the two sides exchange there.
 * The purpose is to confirm over-air formats from our own captures. Pairing
 * identifiers and session addresses are never printed.
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "sniff.h"
#include "radio.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(sniff, LOG_LEVEL_INF);

#define RENDEZVOUS_CHANNEL 2
#define PKT_ADVERTISE 0xE1
#define PKT_POLL 0xE3
#define PKT_INPUT 0xF1
#define PKT_GOODBYE 0xF2
#define PKT_STATUS 0xF3
#define ADVERTISE_PAYLOAD 18
#define RECORD_INPUT 6
#define LISTEN_US 20000U
#define SESSION_QUIET_MS 500U

static const struct radio_addr rendezvous = {
	.base = { 'i', 'b', 'e', 'x' },
	.prefix = 0x10,
};

struct sniff_counts {
	uint32_t packets;
	uint32_t polls;
	uint32_t poll_payloads;
	uint32_t inputs;
	uint32_t input_extra;
	uint32_t status;
	uint32_t goodbye;
	uint32_t advertises;
	uint32_t other;
	uint32_t crc_errors;
	uint32_t sessions;
};

static struct radio_addr session;
static uint8_t session_channel;
static bool on_session;
static uint8_t rx_buf[RADIO_PACKET_SIZE];
static atomic_t print_all;
static struct sniff_counts counts;
static struct k_spinlock lock;

/*
 * The receive wait spins, so the thread runs at the lowest priority: the shell,
 * USB and logging preempt it whenever they have work, at the cost of a few
 * missed packets while they run.
 */
K_THREAD_STACK_DEFINE(sniff_stack, 2048);
static struct k_thread sniff_tid;

/* A bare poll or an input packet that only carries input reports is routine. */
static bool routine(const uint8_t *payload, size_t len)
{
	if (len == 1 && payload[0] == PKT_POLL) {
		return true;
	}
	if (payload[0] == PKT_ADVERTISE && len == ADVERTISE_PAYLOAD) {
		return true;
	}
	if (payload[0] != PKT_INPUT) {
		return false;
	}
	size_t i = 1;

	while (i < len) {
		if (len - i < 2) {
			return false;
		}
		uint8_t rlen = payload[i];
		uint8_t rtype = payload[i + 1];

		if (rlen == 0 || rlen > len - i - 2 || rtype != RECORD_INPUT) {
			return false;
		}
		i += (size_t)rlen + 2;
	}
	return true;
}

static void count(const uint8_t *payload, size_t len)
{
	K_SPINLOCK(&lock) {
		counts.packets++;
		switch (payload[0]) {
		case PKT_POLL:
			counts.polls++;
			if (len > 1) {
				counts.poll_payloads++;
			}
			break;
		case PKT_INPUT:
			counts.inputs++;
			if (!routine(payload, len)) {
				counts.input_extra++;
			}
			break;
		case PKT_STATUS:
			counts.status++;
			break;
		case PKT_GOODBYE:
			counts.goodbye++;
			break;
		case PKT_ADVERTISE:
			counts.advertises++;
			break;
		default:
			counts.other++;
			break;
		}
	}
}

static void print_packet(uint32_t t_us, const uint8_t *pkt)
{
	size_t len = pkt[0];
	const uint8_t *payload = pkt + 2;

	printk("%u.%03u rssi=-%u s1=%02x len=%u:", t_us / 1000, t_us % 1000, radio_last_rssi(),
	       pkt[1], (unsigned int)len);
	if (payload[0] == PKT_ADVERTISE && len == ADVERTISE_PAYLOAD) {
		/* Identifiers and the session address stay off the console. */
		printk(" e1 ch=%u (ids and address withheld)\n", payload[9]);
		return;
	}
	for (size_t i = 0; i < len; i++) {
		printk(" %02x", payload[i]);
	}
	printk("\n");
}

static void follow(const uint8_t *payload)
{
	bool changed;

	K_SPINLOCK(&lock) {
		changed = !on_session || session_channel != payload[9] ||
			  memcmp(session.base, &payload[13], 4) != 0 ||
			  session.prefix != payload[17];
		session_channel = payload[9];
		memcpy(session.base, &payload[13], 4);
		session.prefix = payload[17];
		on_session = true;
		if (changed) {
			counts.sessions++;
		}
	}
	if (changed) {
		printk("sniff: following session on channel %u\n", session_channel);
	}
}

static void sniff_thread(void *a, void *b, void *c)
{
	uint32_t last_rx_ms = k_uptime_get_32();

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		struct radio_addr addr;
		uint8_t channel;
		bool session_now;

		K_SPINLOCK(&lock) {
			session_now = on_session;
			addr = session_now ? session : rendezvous;
			channel = session_now ? session_channel : RENDEZVOUS_CHANNEL;
		}
		int rx = radio_receive(channel, &addr, rx_buf, sizeof(rx_buf), LISTEN_US);
		uint32_t now_ms = k_uptime_get_32();

		if (rx == RADIO_RX_OK) {
			const uint8_t *payload = rx_buf + 2;
			size_t len = rx_buf[0];

			last_rx_ms = now_ms;
			count(payload, len);
			if (payload[0] == PKT_ADVERTISE && len == ADVERTISE_PAYLOAD) {
				follow(payload);
			}
			if (atomic_get(&print_all) || !routine(payload, len)) {
				print_packet(radio_now_us(), rx_buf);
			}
		} else if (rx == RADIO_RX_BAD_CRC) {
			K_SPINLOCK(&lock) {
				counts.crc_errors++;
			}
		} else if (rx < 0 && rx != RADIO_RX_NONE && rx != RADIO_RX_INVALID) {
			LOG_WRN("receive failed (%d)", rx);
			k_msleep(10);
		}
		if (session_now && now_ms - last_rx_ms >= SESSION_QUIET_MS) {
			K_SPINLOCK(&lock) {
				on_session = false;
			}
			printk("sniff: session quiet, back to rendezvous\n");
			last_rx_ms = now_ms;
		}
		k_yield();
	}
}

int sniff_start(void)
{
	int err = radio_init();

	if (err) {
		return err;
	}
	k_thread_create(&sniff_tid, sniff_stack, K_THREAD_STACK_SIZEOF(sniff_stack),
			sniff_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
			K_NO_WAIT);
	k_thread_name_set(&sniff_tid, "sniff");
	LOG_INF("passive sniffer running; nothing is transmitted");
	return 0;
}

static int cmd_sniff_status(const struct shell *sh, size_t argc, char **argv)
{
	struct sniff_counts c;
	bool following;
	uint8_t ch;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	K_SPINLOCK(&lock) {
		c = counts;
		following = on_session;
		ch = session_channel;
	}
	shell_print(sh, "%s%u print_all=%d", following ? "following session on channel " :
		    "waiting on rendezvous, last channel ", ch, (int)atomic_get(&print_all));
	shell_print(sh, "packets=%u polls=%u poll_payloads=%u inputs=%u input_extra=%u",
		    c.packets, c.polls, c.poll_payloads, c.inputs, c.input_extra);
	shell_print(sh, "status=%u goodbye=%u advertises=%u other=%u crc_errors=%u sessions=%u",
		    c.status, c.goodbye, c.advertises, c.other, c.crc_errors, c.sessions);
	return 0;
}

static int cmd_sniff_all(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
		shell_error(sh, "use: sniff all on|off");
		return -EINVAL;
	}
	atomic_set(&print_all, strcmp(argv[1], "on") == 0);
	return 0;
}

static int cmd_sniff_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	K_SPINLOCK(&lock) {
		memset(&counts, 0, sizeof(counts));
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sniff_cmds,
	SHELL_CMD_ARG(status, NULL, "Packet counters and what is being followed", cmd_sniff_status, 1, 0),
	SHELL_CMD_ARG(all, NULL, "all on|off: also print bare polls and plain input", cmd_sniff_all, 2, 0),
	SHELL_CMD_ARG(reset, NULL, "Clear the counters", cmd_sniff_reset, 1, 0),
	SHELL_SUBCMD_SET_END
);
SHELL_CMD_REGISTER(sniff, &sniff_cmds, "Passive link sniffer", NULL);
