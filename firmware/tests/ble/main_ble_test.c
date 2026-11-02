/* SPDX-License-Identifier: Apache-2.0 */
#include "../../../tests/receiver/test_support.h"
#define main firmware_main
#include "../../src/main.c"
#undef main

static int storage_error, host_error, radio_error, starts;
static unsigned sent;
static struct gamepad_report last;

int bond_init(void) { return storage_error; }
int bond_snapshot(int slot, struct bond *out)
{
	(void)slot;
	*out = (struct bond){0};
	return 0;
}
int ble_gamepad_start(void) { return host_error; }
int ble_gamepad_update(const struct gamepad_report *report)
{
	last = *report;
	sent++;
	return 0;
}
int link_start(link_input_cb input, link_lost_cb lost)
{
	CHECK(input == on_input && lost == on_lost);
	starts++;
	return radio_error;
}
bool link_any_connected(void) { return false; }
void gamepad_neutral(struct gamepad_report *report)
{
	*report = (struct gamepad_report){ .hat = 8 };
}
void gamepad_from_ctrl(const struct ctrl_state *state, struct gamepad_report *report)
{
	gamepad_neutral(report);
	report->buttons = (uint16_t)state->buttons;
}

int main(void)
{
	struct ctrl_state input = { .buttons = BTN_A };

	on_input(0, &input);
	CHECK(sent == 1 && last.buttons == BTN_A);
	input.buttons = BTN_B;
	on_input(1, &input);
	CHECK(sent == 1);
	on_lost(1);
	CHECK(sent == 1);
	on_lost(0);
	CHECK(sent == 2 && last.buttons == 0 && last.hat == 8);
	on_input(1, &input);
	CHECK(sent == 3 && last.buttons == BTN_B);

	storage_error = -EIO;
	CHECK(firmware_main() == -EIO && starts == 0);
	storage_error = 0;
	host_error = -ENODEV;
	CHECK(firmware_main() == -ENODEV && starts == 0);
	host_error = 0;
	radio_error = -ETIMEDOUT;
	CHECK(firmware_main() == -ETIMEDOUT && starts == 1);
	puts("BLE bridge main: actual input, sticky owner, neutral release and startup failures passed");
	return 0;
}
