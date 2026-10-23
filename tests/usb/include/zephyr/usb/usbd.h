/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct device {
	int unused;
};

extern const struct device test_device;
#define DEVICE_DT_GET(node) (&test_device)
#define DT_NODELABEL(label) label
#define DT_NUM_INST_STATUS_OKAY(compat) 1
#define DT_PROP(node, prop) TEST_DT_##prop
#if defined(CONFIG_PUCKOFF_USB_SWITCH)
#define TEST_DT_in_report_size 64
#define TEST_DT_out_report_size 64
#define TEST_DT_in_polling_period_us 5000
#define TEST_DT_out_polling_period_us 5000
#define DT_NODE_HAS_PROP(node, prop) 1
#else
#define TEST_DT_in_report_size 16
#define DT_NODE_HAS_PROP(node, prop) 0
#endif

enum usbd_speed { USBD_SPEED_FS };
enum usbd_msg_type {
	USBD_MSG_SUSPEND, USBD_MSG_RESUME, USBD_MSG_VBUS_READY, USBD_MSG_VBUS_REMOVED
};
struct usbd_msg {
	enum usbd_msg_type type;
};
struct usbd_context {
	uint16_t vid, pid;
	uint8_t class_code, subclass, protocol;
};
struct usbd_desc_node {
	const char *text;
};
struct usbd_config_node {
	unsigned int power;
};
typedef void (*test_usbd_msg_cb)(struct usbd_context *, const struct usbd_msg *);

#define USB_BCC_MISCELLANEOUS 0xEF
#define USBD_DEVICE_DEFINE(name, dev, vendor, product) \
	struct usbd_context name = { .vid = vendor, .pid = product, \
		.class_code = 0xEF, .subclass = 2, .protocol = 1 }
#define USBD_DESC_LANG_DEFINE(name) struct usbd_desc_node name
#define USBD_DESC_MANUFACTURER_DEFINE(name, value) \
	struct usbd_desc_node name = { .text = value }
#define USBD_DESC_PRODUCT_DEFINE(name, value) \
	struct usbd_desc_node name = { .text = value }
#define USBD_DESC_SERIAL_NUMBER_DEFINE(name) struct usbd_desc_node name
#define USBD_DESC_CONFIG_DEFINE(name, value) \
	struct usbd_desc_node name = { .text = value }
#define USBD_CONFIGURATION_DEFINE(name, attrs, max_power, desc) \
	struct usbd_config_node name = { .power = max_power }

bool device_is_ready(const struct device *dev);
bool usbd_can_detect_vbus(struct usbd_context *ctx);
int usbd_enable(struct usbd_context *ctx);
int usbd_disable(struct usbd_context *ctx);
int usbd_init(struct usbd_context *ctx);
int usbd_add_descriptor(struct usbd_context *ctx, struct usbd_desc_node *desc);
int usbd_add_configuration(struct usbd_context *ctx, enum usbd_speed speed,
			    struct usbd_config_node *cfg);
int usbd_register_class(struct usbd_context *ctx, const char *name, enum usbd_speed speed,
			uint8_t config);
int usbd_register_all_classes(struct usbd_context *ctx, enum usbd_speed speed,
			      uint8_t config, const char *const *exclude);
void usbd_device_set_code_triple(struct usbd_context *ctx, enum usbd_speed speed,
				 uint8_t class_code, uint8_t subclass, uint8_t protocol);
int usbd_msg_register_cb(struct usbd_context *ctx, test_usbd_msg_cb cb);
