/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
struct onoff_manager { int unused; };
struct sys_notify { int unused; };
struct onoff_client { int node; struct sys_notify notify; };
#define CLOCK_CONTROL_NRF_SUBSYS_HF 1
struct onoff_manager *z_nrf_clock_control_get_onoff(int clock);
void sys_notify_init_spinwait(struct sys_notify *notify);
int onoff_request(struct onoff_manager *manager, struct onoff_client *client);
int sys_notify_fetch_result(struct sys_notify *notify, int *result);
int onoff_cancel_or_release(struct onoff_manager *manager, struct onoff_client *client);
