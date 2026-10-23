/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors. */
#include "usb.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
#include <hal/nrf_power.h>
#include <zephyr/sys/reboot.h>
#endif

#if defined(CONFIG_PUCKOFF_USB_SWITCH)
#include "usb_switch.h"
#define PUCKOFF_VID 0x0F0D
#define PUCKOFF_PID 0x0092
#define PUCKOFF_PRODUCT "PuckOff Wired Switch (experimental)"
#define USB_INPUT_SIZE SWITCH_REPORT_SIZE
#define USB_REPORT_DESC switch_hid_desc
#define USB_REPORT_DESC_LEN switch_hid_desc_len

BUILD_ASSERT(!IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS), "Switch mode must be HID-only");
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(zephyr_hid_device) == 1, "One Switch gamepad only");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(gamepad), in_report_size) == 64 &&
	     DT_PROP(DT_NODELABEL(gamepad), out_report_size) == 64 &&
	     DT_PROP(DT_NODELABEL(gamepad), in_polling_period_us) == 5000 &&
	     DT_PROP(DT_NODELABEL(gamepad), out_polling_period_us) == 5000,
	     "Use modes/switch.conf together with modes/switch.overlay");
#else
/*
 * pid.codes 0001 is private-test-only, not unique or allocated to PuckOff.
 * Do not redistribute, sell, or manufacture devices with this identity.
 */
#define PUCKOFF_VID 0x1209
#define PUCKOFF_PID 0x0001
#define PUCKOFF_PRODUCT "PuckOff Receiver"
#define USB_INPUT_SIZE GAMEPAD_REPORT_SIZE
#define USB_REPORT_DESC gamepad_hid_desc
#define USB_REPORT_DESC_LEN gamepad_hid_desc_len

BUILD_ASSERT(!DT_NODE_HAS_PROP(DT_NODELABEL(gamepad), out_report_size),
	     "The generic input-only descriptor must not use the Switch overlay");
#endif

BUILD_ASSERT(DT_PROP(DT_NODELABEL(gamepad), in_report_size) >= USB_INPUT_SIZE,
	     "HID endpoint is too small");
BUILD_ASSERT(!IS_ENABLED(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT),
	     "PuckOff must initialize its own USB device, not the board CDC helper");

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

#define USB_RETRY_MS 4

USBD_DEVICE_DEFINE(puckoff_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), PUCKOFF_VID,
		   PUCKOFF_PID);
USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr, "PuckOff");
USBD_DESC_PRODUCT_DEFINE(product, PUCKOFF_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(sn);
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "Default");
USBD_CONFIGURATION_DEFINE(fs_config, 0, 100, &fs_cfg_desc);

static const struct device *hid_dev = DEVICE_DT_GET(DT_NODELABEL(gamepad));
static bool hid_ready;
static bool suspended;
static uint8_t pending[USB_INPUT_SIZE];
static bool have_pending;
static bool in_flight;
static uint8_t tx_report[USB_INPUT_SIZE] __aligned(sizeof(void *));
static uint32_t idle_ms;
static int64_t last_report_ms;
static int64_t retry_at_ms;
static int submit_error;
static struct k_spinlock lock;

#if defined(CONFIG_PUCKOFF_USB_SWITCH)
static uint8_t output_state[SWITCH_OUTPUT_SIZE];
#endif

static void tx_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tx_work, tx_work_handler);

#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
static void return_to_uf2(struct k_work *work)
{
	LOG_WRN("development timeout: returning to UF2");
	NRF_POWER->GPREGRET = 0x57;
	sys_reboot(SYS_REBOOT_COLD);
}

static K_WORK_DELAYABLE_DEFINE(recovery_work, return_to_uf2);
#endif

static void encode_report(const struct gamepad_report *in, uint8_t *out)
{
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	switch_encode_report(in, out);
#else
	gamepad_encode_generic(in, out);
#endif
}

static void wake_tx(void)
{
	int err = k_work_reschedule(&tx_work, K_NO_WAIT);

	if (err < 0) {
		LOG_ERR("cannot schedule HID input (%d)", err);
	}
}

static void schedule_tx(int64_t delay_ms)
{
	/* Do not postpone a newer, immediate wakeup queued during this worker. */
	int err = k_work_schedule(&tx_work, K_MSEC(delay_ms));

	if (err < 0) {
		LOG_ERR("cannot schedule HID retry/idle (%d)", err);
	}
}

static void tx_work_handler(struct k_work *work)
{
	bool go = false;
	int64_t now = k_uptime_get();
	int64_t delay_ms = -1;

	K_SPINLOCK(&lock) {
		if (hid_ready && !suspended && !in_flight) {
			if (now < retry_at_ms) {
				delay_ms = retry_at_ms - now;
			} else if (have_pending || (idle_ms && now - last_report_ms >= idle_ms)) {
				memcpy(tx_report, pending, sizeof(tx_report));
				have_pending = false;
				in_flight = true;
				go = true;
			} else if (idle_ms) {
				delay_ms = idle_ms - (now - last_report_ms);
			}
		}
	}
	if (!go) {
		if (delay_ms >= 0) {
			schedule_tx(delay_ms);
		}
		return;
	}

	/* Only this worker submits. Callbacks merely release ownership and wake it. */
	int err = hid_device_submit_report(hid_dev, sizeof(tx_report), tx_report);
	bool changed;

	K_SPINLOCK(&lock) {
		changed = submit_error != err;
		submit_error = err;
		if (err) {
			/* A failed submit has no completion callback. Keep the newest state. */
			in_flight = false;
			have_pending = true;
			retry_at_ms = k_uptime_get() + USB_RETRY_MS;
		} else {
			retry_at_ms = 0;
		}
	}
	if (err) {
		if (changed) {
			LOG_WRN("HID submit failed (%d); retaining latest state", err);
		}
		schedule_tx(USB_RETRY_MS);
	} else if (changed) {
		LOG_INF("HID input recovered");
	}
}

static void iface_ready(const struct device *dev, const bool ready)
{
	K_SPINLOCK(&lock) {
		hid_ready = ready;
		have_pending = true;
		suspended = false;
		retry_at_ms = 0;
		/* Disable is not completion: the stack may still own tx_report. */
	}
	LOG_INF("gamepad %s", ready ? "ready" : "stopped");
	wake_tx();
}

static void report_done(const struct device *dev, const uint8_t *const report)
{
	bool expected = false;

	K_SPINLOCK(&lock) {
		if (in_flight && report == tx_report) {
			expected = true;
			in_flight = false;
			last_report_ms = k_uptime_get();
		}
	}
	if (!expected) {
		LOG_ERR("unexpected HID completion");
		return;
	}
	wake_tx();
}

static int get_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, uint8_t *const buf)
{
	if (id != 0) {
		return -ENOTSUP;
	}
	if (len == 0) {
		return -EINVAL;
	}
	if (type == HID_REPORT_TYPE_INPUT) {
		uint16_t n = MIN(len, sizeof(pending));

		K_SPINLOCK(&lock) {
			memcpy(buf, pending, n);
		}
		return n;
	}
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	if (type == HID_REPORT_TYPE_OUTPUT) {
		uint16_t n = MIN(len, sizeof(output_state));

		K_SPINLOCK(&lock) {
			memcpy(buf, output_state, n);
		}
		return n;
	}
#endif
	return -ENOTSUP;
}

static void set_idle(const struct device *dev, const uint8_t id, const uint32_t duration)
{
	if (id != 0) {
		/* Zephyr's void idle hooks use the control handler's errno channel. */
		errno = -ENOTSUP;
		return;
	}
	K_SPINLOCK(&lock) {
		idle_ms = duration;
	}
	wake_tx();
}

static uint32_t get_idle(const struct device *dev, const uint8_t id)
{
	uint32_t duration = 0;

	if (id != 0) {
		errno = -ENOTSUP;
		return 0;
	}
	K_SPINLOCK(&lock) {
		duration = idle_ms;
	}
	return duration;
}

#if defined(CONFIG_PUCKOFF_USB_SWITCH)
static int set_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, const uint8_t *const buf)
{
	if (id != 0 || type != HID_REPORT_TYPE_OUTPUT) {
		return -ENOTSUP;
	}
	if (len != sizeof(output_state)) {
		return -EINVAL;
	}
	K_SPINLOCK(&lock) {
		memcpy(output_state, buf, sizeof(output_state));
	}
	/* This vendor report has no implemented actuator (not rumble or player LEDs). */
	return 0;
}

static void output_report(const struct device *dev, const uint16_t len,
			  const uint8_t *const buf)
{
	int err = set_report(dev, HID_REPORT_TYPE_OUTPUT, 0, len, buf);

	if (err) {
		LOG_WRN("invalid wired output report length %u", len);
	}
}
#endif

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready,
	.get_report = get_report,
	.set_idle = set_idle,
	.get_idle = get_idle,
	.input_report_done = report_done,
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	.set_report = set_report,
	.output_report = output_report,
#endif
};

void usb_send_gamepad(const struct gamepad_report *r)
{
	uint8_t next[USB_INPUT_SIZE];

	encode_report(r, next);
	K_SPINLOCK(&lock) {
		if (memcmp(pending, next, sizeof(pending)) != 0) {
			memcpy(pending, next, sizeof(pending));
			have_pending = true;
		}
	}
	wake_tx();
}

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	int err = 0;

	if (msg->type == USBD_MSG_SUSPEND || msg->type == USBD_MSG_RESUME) {
		K_SPINLOCK(&lock) {
			suspended = msg->type == USBD_MSG_SUSPEND;
			have_pending = true;
		}
		wake_tx();
	}
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			err = usbd_enable(ctx);
		} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
			err = usbd_disable(ctx);
		}
	}
	if (err && err != -EALREADY) {
		LOG_ERR("USB power transition failed (%d)", err);
	}
}

int usb_start(void)
{
	struct gamepad_report neutral;
	int err;

#if defined(CONFIG_PUCKOFF_USB_TEST_RECOVERY)
	err = k_work_schedule(&recovery_work, K_MSEC(120000));
	if (err < 0) {
		return err;
	}
#endif
	gamepad_neutral(&neutral);
	encode_report(&neutral, pending);
	if (!device_is_ready(hid_dev)) {
		return -ENODEV;
	}
	err = hid_device_register(hid_dev, USB_REPORT_DESC, USB_REPORT_DESC_LEN, &hid_ops);
	if (err) {
		return err;
	}

	err = usbd_add_descriptor(&puckoff_usbd, &lang);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &mfr);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &product);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &sn);
	err = err ?: usbd_add_configuration(&puckoff_usbd, USBD_SPEED_FS, &fs_config);
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
	err = err ?: usbd_register_class(&puckoff_usbd, "hid_0", USBD_SPEED_FS, 1);
#else
	err = err ?: usbd_register_all_classes(&puckoff_usbd, USBD_SPEED_FS, 1, NULL);
#endif
	if (err) {
		return err;
	}
	/* Zephyr's device macro defaults to the composite/IAD class, even for HID-only. */
	usbd_device_set_code_triple(&puckoff_usbd, USBD_SPEED_FS, 0, 0, 0);
#if !defined(CONFIG_PUCKOFF_USB_SWITCH)
	/* CDC ACM uses an interface association; advertise that at device level. */
	if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS)) {
		usbd_device_set_code_triple(&puckoff_usbd, USBD_SPEED_FS,
					    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	}
#endif
	err = usbd_msg_register_cb(&puckoff_usbd, msg_cb);
	err = err ?: usbd_init(&puckoff_usbd);
	if (err) {
		return err;
	}
	/* Power comes from USB, so the cable is already present: enable now. */
	err = usbd_enable(&puckoff_usbd);
	return err == -EALREADY ? 0 : err;
}
