/* SPDX-License-Identifier: Apache-2.0 */
/* USB device: a serial console (for setup) plus a HID gamepad. */
#include "usb.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb, LOG_LEVEL_INF);

/* pid.codes test VID/PID; replace with an allocated PID before release. */
#define PUCKOFF_VID 0x1209
#define PUCKOFF_PID 0x0001

USBD_DEVICE_DEFINE(puckoff_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), PUCKOFF_VID,
		   PUCKOFF_PID);
USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr, "PuckOff");
USBD_DESC_PRODUCT_DEFINE(product, "PuckOff Receiver");
USBD_DESC_SERIAL_NUMBER_DEFINE(sn);
USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "Default");
USBD_CONFIGURATION_DEFINE(fs_config, 0, 100, &fs_cfg_desc);

static const struct device *hid_dev = DEVICE_DT_GET(DT_NODELABEL(gamepad));
static bool hid_ready;
static struct gamepad_report pending;
static bool have_pending;
static bool in_flight;
static uint8_t tx_report[sizeof(struct gamepad_report)] __aligned(4);
static struct k_spinlock lock;

static void try_send(void)
{
	bool go = false;

	K_SPINLOCK(&lock) {
		if (hid_ready && have_pending && !in_flight) {
			memcpy(tx_report, &pending, sizeof(tx_report));
			have_pending = false;
			in_flight = true;
			go = true;
		}
	}
	if (go && hid_device_submit_report(hid_dev, sizeof(tx_report), tx_report)) {
		K_SPINLOCK(&lock) {
			in_flight = false;
		}
	}
}

static void iface_ready(const struct device *dev, const bool ready)
{
	K_SPINLOCK(&lock) {
		hid_ready = ready;
		in_flight = false;
		if (ready) {
			have_pending = true;
		}
	}
	LOG_INF("gamepad %s", ready ? "ready" : "stopped");
	try_send();
}

static void report_done(const struct device *dev, const uint8_t *const report)
{
	K_SPINLOCK(&lock) {
		in_flight = false;
	}
	try_send();
}

static int get_report(const struct device *dev, const uint8_t type, const uint8_t id,
		      const uint16_t len, uint8_t *const buf)
{
	uint16_t n = MIN(len, sizeof(pending));

	K_SPINLOCK(&lock) {
		memcpy(buf, &pending, n);
	}
	return n;
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = iface_ready,
	.get_report = get_report,
	.input_report_done = report_done,
};

void usb_send_gamepad(const struct gamepad_report *r)
{
	K_SPINLOCK(&lock) {
		pending = *r;
		have_pending = true;
	}
	try_send();
}

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			usbd_enable(ctx);
		} else if (msg->type == USBD_MSG_VBUS_REMOVED) {
			usbd_disable(ctx);
		}
	}
}

int usb_start(void)
{
	int err;

	gamepad_neutral(&pending);
	if (!device_is_ready(hid_dev)) {
		return -ENODEV;
	}
	err = hid_device_register(hid_dev, gamepad_hid_desc, gamepad_hid_desc_len, &hid_ops);
	if (err) {
		return err;
	}

	err = usbd_add_descriptor(&puckoff_usbd, &lang);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &mfr);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &product);
	err = err ?: usbd_add_descriptor(&puckoff_usbd, &sn);
	err = err ?: usbd_add_configuration(&puckoff_usbd, USBD_SPEED_FS, &fs_config);
	err = err ?: usbd_register_all_classes(&puckoff_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		return err;
	}
	/* CDC ACM uses an interface association; advertise that at device level. */
	usbd_device_set_code_triple(&puckoff_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02,
				    0x01);
	err = usbd_msg_register_cb(&puckoff_usbd, msg_cb);
	err = err ?: usbd_init(&puckoff_usbd);
	if (err) {
		return err;
	}
	/* Power comes from USB, so the cable is already present: enable now. */
	err = usbd_enable(&puckoff_usbd);
	return err == -EALREADY ? 0 : err;
}
