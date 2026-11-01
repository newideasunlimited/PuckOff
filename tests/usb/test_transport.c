/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
/* Exercise the real usb.c with deterministic USB/workqueue interleavings. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/class/usbd_hid.h>

#include "usb.h"
#include "usb_switch.h"

#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
#include <hal/nrf_power.h>
#include <zephyr/sys/reboot.h>
#define BACKGROUND_WORK_COUNT 1
struct test_power_registers test_power;
static unsigned int reboot_count;

void sys_reboot(int type)
{
	assert(type == SYS_REBOOT_COLD);
	reboot_count++;
}
#else
#define BACKGROUND_WORK_COUNT 0
#endif

#if defined(CONFIG_PUCKOFF_USB_SWITCH)
#define REPORT_SIZE SWITCH_REPORT_SIZE
#else
#define REPORT_SIZE GAMEPAD_REPORT_SIZE
#endif

const struct device test_device;
static const struct hid_device_ops *ops;
static struct usbd_context *context;
static test_usbd_msg_cb message_cb;
static struct scheduled_work {
	struct k_work_delayable *work;
	int64_t due_ms;
	bool pending;
} queue[2];
static int64_t now_ms;
static const uint8_t *owned_buffer;
static uint8_t owned_snapshot[REPORT_SIZE];
static uint8_t last_accepted[REPORT_SIZE];
static unsigned int accepted, attempts, errors, warnings;
static unsigned int all_class_registrations, hid_only_registrations;
static int fail_submit, fail_count, enable_error;
static bool device_ready = true, worker_running;
static void (*submit_hook)(void);

int64_t k_uptime_get(void)
{
	return now_ms;
}

static struct scheduled_work *work_slot(struct k_work_delayable *work)
{
	for (unsigned int i = 0; i < 2; i++) {
		if (queue[i].work == work) {
			return &queue[i];
		}
	}
	for (unsigned int i = 0; i < 2; i++) {
		if (!queue[i].work) {
			queue[i].work = work;
			return &queue[i];
		}
	}
	assert(!"Too many work items");
	return NULL;
}

int k_work_reschedule(struct k_work_delayable *work, int64_t delay_ms)
{
	struct scheduled_work *slot = work_slot(work);

	assert(delay_ms >= 0);
	slot->pending = true;
	slot->due_ms = now_ms + delay_ms;
	return 1;
}

int k_work_schedule(struct k_work_delayable *work, int64_t delay_ms)
{
	if (work_slot(work)->pending) {
		return 0;
	}
	return k_work_reschedule(work, delay_ms);
}

static unsigned int pending_work_count(void)
{
	return queue[0].pending + queue[1].pending;
}

static void run_work(void)
{
	unsigned int steps = 0;

	assert(!worker_running);
	for (;;) {
		struct scheduled_work *slot = NULL;

		for (unsigned int i = 0; i < 2; i++) {
			if (queue[i].pending && queue[i].due_ms <= now_ms &&
			    (!slot || queue[i].due_ms < slot->due_ms)) {
				slot = &queue[i];
			}
		}
		if (!slot) {
			break;
		}
		struct k_work_delayable *work = slot->work;

		assert(++steps < 100);
		slot->pending = false;
		worker_running = true;
		work->work.handler(&work->work);
		worker_running = false;
	}
}

static void advance(unsigned int ms)
{
	now_ms += ms;
	run_work();
}

void test_log(enum test_log_level level, const char *fmt, ...)
{
	errors += level == TEST_LOG_ERROR;
	warnings += level == TEST_LOG_WARN;
}

bool device_is_ready(const struct device *dev)
{
	return device_ready;
}

bool usbd_can_detect_vbus(struct usbd_context *ctx)
{
	return true;
}

int usbd_enable(struct usbd_context *ctx)
{
	return enable_error;
}

int usbd_disable(struct usbd_context *ctx)
{
	ops->iface_ready(&test_device, false);
	return 0;
}

int usbd_init(struct usbd_context *ctx)
{
	context = ctx;
	return 0;
}

int usbd_add_descriptor(struct usbd_context *ctx, struct usbd_desc_node *desc)
{
	return 0;
}

int usbd_add_configuration(struct usbd_context *ctx, enum usbd_speed speed,
			    struct usbd_config_node *cfg)
{
	assert(speed == USBD_SPEED_FS && cfg->power == 100);
	return 0;
}

int usbd_register_class(struct usbd_context *ctx, const char *name, enum usbd_speed speed,
			uint8_t config)
{
	assert(strcmp(name, "hid_0") == 0 && config == 1 && speed == USBD_SPEED_FS);
	hid_only_registrations++;
	return 0;
}

int usbd_register_all_classes(struct usbd_context *ctx, enum usbd_speed speed,
			      uint8_t config, const char *const *exclude)
{
	assert(config == 1 && speed == USBD_SPEED_FS && exclude == NULL);
	all_class_registrations++;
	return 0;
}

void usbd_device_set_code_triple(struct usbd_context *ctx, enum usbd_speed speed,
				 uint8_t class_code, uint8_t subclass, uint8_t protocol)
{
	ctx->class_code = class_code;
	ctx->subclass = subclass;
	ctx->protocol = protocol;
}

int usbd_msg_register_cb(struct usbd_context *ctx, test_usbd_msg_cb cb)
{
	message_cb = cb;
	return 0;
}

int hid_device_register(const struct device *dev, const uint8_t *desc, uint16_t size,
			 const struct hid_device_ops *callbacks)
{
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	assert(desc == switch_hid_desc && size == switch_hid_desc_len);
#else
	assert(desc == gamepad_hid_desc && size == gamepad_hid_desc_len);
#endif
	ops = callbacks;
	assert(ops->iface_ready && ops->get_report && ops->input_report_done);
	assert(ops->set_idle && ops->get_idle);
	return 0;
}

int hid_device_submit_report(const struct device *dev, uint16_t size, const uint8_t *report)
{
	void (*hook)(void) = submit_hook;

	assert(worker_running);
	assert(size == REPORT_SIZE && (uintptr_t)report % sizeof(void *) == 0);
	attempts++;
	submit_hook = NULL;
	if (fail_count) {
		fail_count--;
		if (hook) {
			hook();
		}
		return fail_submit;
	}
	if (owned_buffer) {
		/* DMA is done, but the callback's net_buf may not have been freed yet. */
		return -ENOMEM;
	}
	owned_buffer = report;
	memcpy(owned_snapshot, report, REPORT_SIZE);
	memcpy(last_accepted, report, REPORT_SIZE);
	accepted++;
	if (hook) {
		hook();
	}
	return 0;
}

static void encode(const struct gamepad_report *report, uint8_t *bytes)
{
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	switch_encode_report(report, bytes);
#else
	gamepad_encode_generic(report, bytes);
#endif
}

static void assert_report(const struct gamepad_report *report)
{
	uint8_t expected[REPORT_SIZE];

	encode(report, expected);
	assert(memcmp(last_accepted, expected, REPORT_SIZE) == 0);
}

static void message(enum usbd_msg_type type)
{
	struct usbd_msg msg = {.type = type};

	message_cb(context, &msg);
}

static void complete(bool run_before_free)
{
	assert(owned_buffer);
	assert(memcmp(owned_buffer, owned_snapshot, REPORT_SIZE) == 0);
	ops->input_report_done(&test_device, owned_buffer);
	if (run_before_free) {
		run_work();
	}
	owned_buffer = NULL;
	run_work();
}

static struct gamepad_report fixture(void)
{
	struct gamepad_report neutral;

	ops->iface_ready(&test_device, false);
	ops->set_idle(&test_device, 0, 0);
	if (owned_buffer) {
		complete(false);
	}
	message(USBD_MSG_RESUME);
	gamepad_neutral(&neutral);
	usb_send_gamepad(&neutral);
	ops->iface_ready(&test_device, true);
	run_work();
	assert(owned_buffer);
	complete(false);
	accepted = attempts = errors = warnings = 0;
	assert(pending_work_count() == BACKGROUND_WORK_COUNT);
	return neutral;
}

static void test_start(void)
{
	device_ready = false;
	assert(usb_start() == -ENODEV);
	device_ready = true;
	assert(usb_start() == 0);
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	assert(context->vid == 0x0F0D && context->pid == 0x0092);
	assert(context->class_code == 0 && context->subclass == 0 && context->protocol == 0);
	assert(hid_only_registrations == 1 && all_class_registrations == 0);
	assert(ops->set_report && ops->output_report);
#else
	assert(context->vid == 0x1209 && context->pid == 1);
	assert(context->class_code == 0xEF && context->subclass == 2 && context->protocol == 1);
	assert(hid_only_registrations == 0 && all_class_registrations == 1);
	assert(!ops->set_report && !ops->output_report);
#endif
}

static void test_latest_snapshot_and_control(void)
{
	struct gamepad_report report = fixture();
	uint8_t buf[66], expected[REPORT_SIZE];

	report.buttons = GAMEPAD_A;
	usb_send_gamepad(&report);
	run_work();
	assert(accepted == 1);
	report.buttons = GAMEPAD_B;
	usb_send_gamepad(&report);
	report.buttons = GAMEPAD_X;
	report.ly = -12345;
	usb_send_gamepad(&report);
	run_work();
	assert(accepted == 1);
	encode(&report, expected);

	for (unsigned int len = 1; len <= 64; len++) {
		unsigned int n = len < REPORT_SIZE ? len : REPORT_SIZE;

		memset(buf, 0xA5, sizeof(buf));
		assert(ops->get_report(&test_device, HID_REPORT_TYPE_INPUT, 0, len, buf + 1) ==
		       (int)n);
		assert(buf[0] == 0xA5 && buf[n + 1] == 0xA5);
		assert(memcmp(buf + 1, expected, n) == 0);
	}
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_INPUT, 0, 0, buf) == -EINVAL);
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_INPUT, 1, 64, buf) == -ENOTSUP);
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_FEATURE, 0, 64, buf) == -ENOTSUP);
	assert(ops->get_report(&test_device, 0xFF, 0, 64, buf) == -ENOTSUP);
#if !defined(CONFIG_PUCKOFF_USB_SWITCH)
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 64, buf) == -ENOTSUP);
#endif
	complete(false);
	assert(accepted == 2);
	assert_report(&report);
	complete(false);
	assert(!owned_buffer);
}

static void test_neutral_retry(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report pressed = neutral;

	pressed.buttons = GAMEPAD_A;
	usb_send_gamepad(&pressed);
	run_work();
	complete(false);
	fail_submit = -EBUSY;
	fail_count = 2;
	usb_send_gamepad(&neutral);
	run_work();
	assert(accepted == 1 && warnings == 1);
	advance(3);
	assert(attempts == 2);
	/* New calls cannot defeat the four-millisecond failure backoff. */
	usb_send_gamepad(&neutral);
	run_work();
	assert(attempts == 2);
	advance(1);
	assert(attempts == 3 && accepted == 1 && warnings == 1);
	advance(4);
	assert(accepted == 2);
	assert_report(&neutral);
	complete(false);
	assert(!owned_buffer && pending_work_count() == BACKGROUND_WORK_COUNT);
}

static void replace_with_neutral(void)
{
	struct gamepad_report neutral;

	gamepad_neutral(&neutral);
	usb_send_gamepad(&neutral);
}

static void test_update_during_failed_submit(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report report = neutral;

	report.buttons = GAMEPAD_A;
	fail_submit = -EAGAIN;
	fail_count = 1;
	submit_hook = replace_with_neutral;
	usb_send_gamepad(&report);
	run_work();
	assert(!accepted);
	advance(4);
	assert(accepted == 1);
	assert_report(&neutral);
	complete(false);
}

static void test_reconnect_with_outstanding_report(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report report = neutral;
	uint8_t wrong_buffer[REPORT_SIZE] = {0};

	report.buttons = GAMEPAD_A;
	usb_send_gamepad(&report);
	run_work();
	assert(owned_buffer);
	ops->iface_ready(&test_device, false);
	usb_send_gamepad(&neutral);
	ops->iface_ready(&test_device, true);
	run_work();
	assert(accepted == 1);
	assert(memcmp(owned_buffer, owned_snapshot, REPORT_SIZE) == 0);
	ops->input_report_done(&test_device, wrong_buffer);
	run_work();
	assert(errors == 1 && accepted == 1);
	complete(false);
	assert(accepted == 2);
	assert_report(&neutral);
	complete(false);
	ops->iface_ready(&test_device, false);
	run_work();
	advance(100);
	assert(accepted == 2);
	ops->iface_ready(&test_device, true);
	run_work();
	assert(accepted == 3);
	assert_report(&neutral);
	complete(false);
}

static void test_pool_not_released_at_callback(void)
{
	struct gamepad_report report = fixture();

	report.buttons = GAMEPAD_A;
	usb_send_gamepad(&report);
	run_work();
	report.buttons = GAMEPAD_B;
	usb_send_gamepad(&report);
	complete(true);
	assert(accepted == 1 && warnings == 1 && !owned_buffer);
	advance(4);
	assert(accepted == 2);
	assert_report(&report);
	complete(false);
}

static void finish_inside_submit(void)
{
	ops->input_report_done(&test_device, owned_buffer);
	owned_buffer = NULL;
	replace_with_neutral();
}

static void test_completion_before_submit_returns(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report report = neutral;

	report.buttons = GAMEPAD_A;
	submit_hook = finish_inside_submit;
	usb_send_gamepad(&report);
	run_work();
	assert(accepted == 2);
	assert_report(&neutral);
	complete(false);
}

static void test_idle_and_suspend(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report report = neutral;

	usb_send_gamepad(&neutral);
	run_work();
	advance(20);
	assert(accepted == 0);
	ops->set_idle(&test_device, 0, 8);
	run_work();
	assert(accepted == 1); /* Already idle for more than eight milliseconds. */
	complete(false);
	advance(7);
	assert(accepted == 1);
	advance(1);
	assert(accepted == 2);
	complete(false);
	errno = 0;
	ops->set_idle(&test_device, 1, 0);
	assert(errno == -ENOTSUP && ops->get_idle(&test_device, 0) == 8);
	errno = 0;
	assert(ops->get_idle(&test_device, 2) == 0 && errno == -ENOTSUP);
	ops->set_idle(&test_device, 0, 0);
	report.buttons = GAMEPAD_A;
	usb_send_gamepad(&report);
	run_work();
	assert(accepted == 3);
	message(USBD_MSG_SUSPEND);
	usb_send_gamepad(&neutral);
	complete(false);
	advance(20);
	assert(accepted == 3);
	message(USBD_MSG_RESUME);
	run_work();
	assert(accepted == 4);
	assert_report(&neutral);
	complete(false);
	advance(20);
	assert(accepted == 4);
}

static void test_disconnect_during_retry(void)
{
	struct gamepad_report report = fixture();

	report.buttons = GAMEPAD_A;
	fail_submit = -EACCES;
	fail_count = 1;
	usb_send_gamepad(&report);
	run_work();
	message(USBD_MSG_VBUS_REMOVED);
	advance(100);
	assert(accepted == 0);
	ops->iface_ready(&test_device, true);
	run_work();
	assert(accepted == 1);
	assert_report(&report);
	complete(false);
	enable_error = -EIO;
	message(USBD_MSG_VBUS_READY);
	assert(errors == 1);
	enable_error = -EALREADY;
	message(USBD_MSG_VBUS_READY);
	assert(errors == 1);
	enable_error = 0;
}

static void test_reset_after_deferred_suspend(void)
{
	struct gamepad_report neutral = fixture();
	struct gamepad_report report = neutral;

	report.buttons = GAMEPAD_A;
	usb_send_gamepad(&report);
	run_work();
	assert(accepted == 1 && owned_buffer);
	ops->iface_ready(&test_device, false);
	ops->iface_ready(&test_device, true);
	message(USBD_MSG_SUSPEND);
	usb_send_gamepad(&neutral);
	message(USBD_MSG_RESET);
	run_work();
	assert(accepted == 1 && owned_buffer);
	assert(memcmp(owned_buffer, owned_snapshot, REPORT_SIZE) == 0);
	complete(false);
	assert(accepted == 2);
	assert_report(&neutral);
	complete(false);

	/* RESET must clear suspend, not invent an active configuration. */
	ops->iface_ready(&test_device, false);
	message(USBD_MSG_SUSPEND);
	report.buttons = GAMEPAD_B;
	usb_send_gamepad(&report);
	message(USBD_MSG_RESET);
	run_work();
	assert(accepted == 2 && !owned_buffer);
	ops->iface_ready(&test_device, true);
	run_work();
	assert(accepted == 3);
	assert_report(&report);
	complete(false);
}

#if defined(CONFIG_PUCKOFF_USB_SWITCH)
static void test_output_reports(void)
{
	uint8_t input[64], result[64];

	fixture();
	memset(input, 0x42, sizeof(input));
	assert(ops->set_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 8, input) == 0);
	memset(result, 0xA5, sizeof(result));
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 64, result) == 8);
	assert(memcmp(input, result, 8) == 0 && result[8] == 0xA5);
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 3, result) == 3);
	assert(ops->set_report(&test_device, HID_REPORT_TYPE_OUTPUT, 1, 8, input) == -ENOTSUP);
	assert(ops->set_report(&test_device, HID_REPORT_TYPE_INPUT, 0, 8, input) == -ENOTSUP);
	assert(ops->set_report(&test_device, HID_REPORT_TYPE_FEATURE, 0, 8, input) == -ENOTSUP);
	for (unsigned int len = 0; len <= 64; len++) {
		assert(ops->set_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, len, input) ==
		       (len == 8 ? 0 : -EINVAL));
	}
	memset(input, 0x17, sizeof(input));
	ops->output_report(&test_device, 8, input);
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 8, result) == 8);
	assert(memcmp(input, result, 8) == 0);
	memset(input, 0x99, sizeof(input));
	ops->output_report(&test_device, 64, input);
	assert(warnings == 1);
	assert(ops->get_report(&test_device, HID_REPORT_TYPE_OUTPUT, 0, 8, result) == 8);
	assert(result[0] == 0x17);
}
#endif

#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
static void test_timed_recovery(void)
{
	fixture();
	assert(reboot_count == 0 && test_power.GPREGRET == 0);
	assert(now_ms < 120000);
	advance((unsigned int)(120000 - now_ms - 1));
	assert(reboot_count == 0 && test_power.GPREGRET == 0);
	advance(1);
	assert(reboot_count == 1 && test_power.GPREGRET == 0x57);
}
#endif

int main(void)
{
	test_start();
	test_latest_snapshot_and_control();
	test_neutral_retry();
	test_update_during_failed_submit();
	test_reconnect_with_outstanding_report();
	test_pool_not_released_at_callback();
	test_completion_before_submit_returns();
	test_idle_and_suspend();
	test_disconnect_during_retry();
	test_reset_after_deferred_suspend();
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	test_output_reports();
#endif
#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
	test_timed_recovery();
#endif
	puts("USB control, retry, idle, suspend and buffer-ownership tests passed");
	return 0;
}
