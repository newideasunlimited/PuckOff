/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Copyright 2026 New Ideas Unlimited, LLC and the PuckOff contributors.
 */
#include "gamepad.h"
#include "report_state.h"

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ble_gamepad, LOG_LEVEL_INF);

BUILD_ASSERT(CONFIG_BT_MAX_CONN == 1, "BLE report gates require exactly one host");
BUILD_ASSERT(sizeof(struct gamepad_report) == BLE_GAMEPAD_REPORT_SIZE);

#define REPORT_INTERVAL_MS 8
#define INPUT_REPORT_ATTR 6

static K_MUTEX_DEFINE(state_lock);
static struct ble_report_state state;
static struct ble_gamepad_status counters;
static struct bt_conn *host;
static bool started;
static int last_tx_error;

static void report_work_handler(struct k_work *work);
static void advertise_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(report_work, report_work_handler);
static K_WORK_DELAYABLE_DEFINE(advertise_work, advertise_work_handler);

static ssize_t read_information(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	/* HID 1.11, no country specialization, normally connectable, no remote wake. */
	static const uint8_t information[] = { 0x11, 0x01, 0x00, 0x02 };

	return bt_gatt_attr_read(conn, attr, buf, len, offset, information, sizeof(information));
}

static ssize_t read_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, gamepad_hid_desc,
				 gamepad_hid_desc_len);
}

static ssize_t read_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	uint8_t report[BLE_GAMEPAD_REPORT_SIZE];

	k_mutex_lock(&state_lock, K_FOREVER);
	memcpy(report, state.report, sizeof(report));
	k_mutex_unlock(&state_lock);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, report, sizeof(report));
}

static ssize_t read_reference(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      void *buf, uint16_t len, uint16_t offset)
{
	/* The shared report map has no Report ID item; input report ID is zero. */
	static const uint8_t reference[] = { 0, 1 };

	return bt_gatt_attr_read(conn, attr, buf, len, offset, reference, sizeof(reference));
}

static void subscription_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	k_mutex_lock(&state_lock, K_FOREVER);
	ble_report_gate_set(&state, BLE_REPORT_SUBSCRIBED, value == BT_GATT_CCC_NOTIFY);
	k_mutex_unlock(&state_lock);
	k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
}

static ssize_t write_control(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *value = buf;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (value[0] > 1) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	ble_report_gate_set(&state, BLE_REPORT_SUSPENDED, value[0] == 0);
	k_mutex_unlock(&state_lock);
	k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
	return len;
}

BT_GATT_SERVICE_DEFINE(gamepad_service,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT, read_information, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_ENCRYPT, read_map, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ_ENCRYPT, read_report, NULL, NULL),
	BT_GATT_CCC(subscription_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
	BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT,
			   read_reference, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_control, NULL)
);

static void report_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (host) {
		bool subscribed = bt_gatt_is_subscribed(host,
						       &gamepad_service.attrs[INPUT_REPORT_ATTR],
						       BT_GATT_CCC_NOTIFY);

		if (subscribed != ((state.gates & BLE_REPORT_SUBSCRIBED) != 0)) {
			ble_report_gate_set(&state, BLE_REPORT_SUBSCRIBED, subscribed);
		}
	}
	if (!host || !ble_report_ready(&state)) {
		k_mutex_unlock(&state_lock);
		return;
	}
	struct bt_gatt_notify_params params = {
		.attr = &gamepad_service.attrs[INPUT_REPORT_ATTR],
		.data = state.report,
		.len = sizeof(state.report),
	};

	/* On the system workqueue this returns -ENOMEM rather than waiting for ATT
	 * buffers. The stack copies the bytes; at most one newest report is staged.
	 */
	err = bt_gatt_notify_cb(host, &params);
	if (!err) {
		state.pending = false;
		counters.submitted++;
		last_tx_error = 0;
	} else if (err == -ENOMEM || err == -EAGAIN) {
		counters.backpressure++;
	} else {
		counters.failures++;
		if (err != last_tx_error) {
			LOG_WRN("HID notification failed (%d)", err);
			last_tx_error = err;
		}
	}
	if (err) {
		k_work_reschedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
	}
	k_mutex_unlock(&state_lock);
}

static const struct bt_data advertising[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, 0xc4, 0x03),
};

static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertise_work_handler(struct k_work *work)
{
	int err;

	ARG_UNUSED(work);
	k_mutex_lock(&state_lock, K_FOREVER);
	if (!started || host) {
		k_mutex_unlock(&state_lock);
		return;
	}
	err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, advertising, ARRAY_SIZE(advertising),
			      scan_response, ARRAY_SIZE(scan_response));
	k_mutex_unlock(&state_lock);
	if (err && err != -EALREADY) {
		LOG_ERR("BLE advertising failed (%d); retrying", err);
		k_work_reschedule(&advertise_work, K_SECONDS(1));
	}
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	int rc;

	if (err) {
		LOG_WRN("BLE connection failed (%u)", err);
		k_work_reschedule(&advertise_work, K_MSEC(100));
		return;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	ble_report_reset(&state);
	ble_report_gate_set(&state, BLE_REPORT_CONNECTED, true);
	host = bt_conn_ref(conn);
	k_mutex_unlock(&state_lock);

	rc = bt_conn_set_security(conn, BT_SECURITY_L2);
	if (rc) {
		LOG_ERR("Could not request BLE encryption (%d)", rc);
		rc = bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		if (rc) {
			LOG_ERR("Could not disconnect insecure host (%d)", rc);
		}
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn *old;

	ARG_UNUSED(conn);
	k_mutex_lock(&state_lock, K_FOREVER);
	old = host;
	host = NULL;
	ble_report_reset(&state);
	k_mutex_unlock(&state_lock);
	if (old) {
		bt_conn_unref(old);
	}
	LOG_INF("BLE host disconnected (%u)", reason);
}

static void recycled(void)
{
	/* With one connection, advertising must wait for the pool slot to be freed. */
	k_work_reschedule(&advertise_work, K_NO_WAIT);
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
			     enum bt_security_err err)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	ble_report_gate_set(&state, BLE_REPORT_ENCRYPTED, !err && level >= BT_SECURITY_L2);
	k_mutex_unlock(&state_lock);
	if (err) {
		LOG_WRN("BLE security failed (%u)", err);
		int rc = bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);

		if (rc) {
			LOG_ERR("Could not disconnect host after security failure (%d)", rc);
		}
		return;
	}
	k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
}

BT_CONN_CB_DEFINE(gamepad_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

int ble_gamepad_start(void)
{
	int err;

	if (started) {
		return -EALREADY;
	}
	ble_report_reset(&state);
	err = settings_subsys_init();
	if (err) {
		return err;
	}
	err = bt_enable(NULL);
	if (err) {
		return err;
	}
	err = settings_load_subtree("bt");
	if (err) {
		LOG_ERR("BLE host settings failed (%d); not advertising", err);
		return err;
	}
	k_mutex_lock(&state_lock, K_FOREVER);
	started = true;
	k_mutex_unlock(&state_lock);
	k_work_schedule(&advertise_work, K_NO_WAIT);
	return 0;
}

int ble_gamepad_update(const struct gamepad_report *report)
{
	uint8_t encoded[BLE_GAMEPAD_REPORT_SIZE];

	if (!report) {
		LOG_ERR("Missing BLE gamepad report");
		return -EINVAL;
	}
	if (report->hat > 8) {
		LOG_ERR("Invalid gamepad hat (%u)", report->hat);
		return -EINVAL;
	}
	sys_put_le16(report->buttons, encoded);
	encoded[2] = report->hat;
	sys_put_le16((uint16_t)report->lx, &encoded[3]);
	sys_put_le16((uint16_t)report->ly, &encoded[5]);
	sys_put_le16((uint16_t)report->rx, &encoded[7]);
	sys_put_le16((uint16_t)report->ry, &encoded[9]);
	encoded[11] = report->lt;
	encoded[12] = report->rt;

	k_mutex_lock(&state_lock, K_FOREVER);
	if (!started) {
		k_mutex_unlock(&state_lock);
		return -EAGAIN;
	}
	ble_report_update(&state, encoded);
	k_mutex_unlock(&state_lock);
	k_work_schedule(&report_work, K_MSEC(REPORT_INTERVAL_MS));
	return 0;
}

void ble_gamepad_get_status(struct ble_gamepad_status *status)
{
	k_mutex_lock(&state_lock, K_FOREVER);
	*status = counters;
	status->connected = (state.gates & BLE_REPORT_CONNECTED) != 0;
	status->encrypted = (state.gates & BLE_REPORT_ENCRYPTED) != 0;
	status->subscribed = (state.gates & BLE_REPORT_SUBSCRIBED) != 0;
	status->suspended = (state.gates & BLE_REPORT_SUSPENDED) != 0;
	k_mutex_unlock(&state_lock);
}

int ble_gamepad_forget_host(void)
{
	int err;

	k_mutex_lock(&state_lock, K_FOREVER);
	if (host) {
		k_mutex_unlock(&state_lock);
		return -EBUSY;
	}
	err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	k_mutex_unlock(&state_lock);
	return err;
}
