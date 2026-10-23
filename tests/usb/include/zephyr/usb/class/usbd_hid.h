/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/hid.h>

enum {
	HID_REPORT_TYPE_INPUT = 1,
	HID_REPORT_TYPE_OUTPUT = 2,
	HID_REPORT_TYPE_FEATURE = 3,
};

struct hid_device_ops {
	void (*iface_ready)(const struct device *, bool);
	int (*get_report)(const struct device *, uint8_t, uint8_t, uint16_t, uint8_t *);
	int (*set_report)(const struct device *, uint8_t, uint8_t, uint16_t, const uint8_t *);
	void (*set_idle)(const struct device *, uint8_t, uint32_t);
	uint32_t (*get_idle)(const struct device *, uint8_t);
	void (*input_report_done)(const struct device *, const uint8_t *);
	void (*output_report)(const struct device *, uint16_t, const uint8_t *);
};

int hid_device_register(const struct device *dev, const uint8_t *desc, uint16_t size,
			 const struct hid_device_ops *ops);
int hid_device_submit_report(const struct device *dev, uint16_t size, const uint8_t *report);
