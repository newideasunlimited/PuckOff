/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include <string.h>
#define main firmware_main
#include "../../firmware/src/main.c"
#undef main

static int storage_error, usb_error, link_error, link_calls, usb_calls;
static struct gamepad_report sent[8];
static unsigned sent_count;

int bond_init(void) { return storage_error; }
int bond_snapshot(int slot, struct bond *out)
{
	(void)slot; memset(out, 0, sizeof(*out)); return 0;
}
int usb_start(void) { usb_calls++; return usb_error; }
int link_start(link_input_cb on_report, link_lost_cb on_disconnect)
{
	CHECK(on_report == on_input && on_disconnect == on_lost);
	link_calls++;
	return link_error;
}
bool link_any_connected(void) { return false; }
void gamepad_neutral(struct gamepad_report *out)
{
	memset(out, 0, sizeof(*out)); out->hat = 8;
}
void gamepad_from_ctrl(const struct ctrl_state *in, struct gamepad_report *out)
{
	gamepad_neutral(out); out->buttons = (uint16_t)in->buttons;
}
void usb_send_gamepad(const struct gamepad_report *r)
{
	CHECK(sent_count < 8);
	sent[sent_count++] = *r;
}

int main(void)
{
	struct ctrl_state a = {.buttons = BTN_A}, b = {.buttons = BTN_B};

	on_input(0, &a);
	on_input(1, &b);
	CHECK(sent_count == 1 && sent[0].buttons == BTN_A && active_slot == 0);
	on_lost(1);
	CHECK(sent_count == 1 && active_slot == 0);
	on_lost(0);
	CHECK(sent_count == 2 && sent[1].buttons == 0 && sent[1].hat == 8);
	CHECK(active_slot == -1);
	on_input(1, &b);
	CHECK(sent_count == 3 && sent[2].buttons == BTN_B && active_slot == 1);
	main_set_watch(true);
	CHECK(atomic_get(&watch));
	main_set_watch(false);
	CHECK(!atomic_get(&watch));

	storage_error = -EIO;
	CHECK(firmware_main() == -EIO && link_calls == 0 && usb_calls == 1);
	storage_error = 0; usb_error = -ENODEV;
	CHECK(firmware_main() == -ENODEV && link_calls == 0);
	usb_error = 0; link_error = -ETIMEDOUT;
	CHECK(firmware_main() == -ETIMEDOUT && link_calls == 1);
	CHECK(test_ready_logs == 0);
	puts("main: sticky gamepad, neutral-before-handoff and explicit startup failures");
	return 0;
}
