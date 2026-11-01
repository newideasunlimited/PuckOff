/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

typedef uint8_t mpsl_timeslot_session_id_t;
enum {
	MPSL_TIMESLOT_SIGNAL_START,
	MPSL_TIMESLOT_SIGNAL_TIMER0,
	MPSL_TIMESLOT_SIGNAL_RADIO,
	MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED,
	MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED,
	MPSL_TIMESLOT_SIGNAL_BLOCKED,
	MPSL_TIMESLOT_SIGNAL_CANCELLED,
	MPSL_TIMESLOT_SIGNAL_SESSION_IDLE,
	MPSL_TIMESLOT_SIGNAL_INVALID_RETURN,
	MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED,
	MPSL_TIMESLOT_SIGNAL_OVERSTAYED,
};
enum {
	MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
	MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
	MPSL_TIMESLOT_SIGNAL_ACTION_END,
	MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST,
};
enum { MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED, MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE };
enum { MPSL_TIMESLOT_PRIORITY_HIGH, MPSL_TIMESLOT_PRIORITY_NORMAL };
enum { MPSL_TIMESLOT_REQ_TYPE_EARLIEST, MPSL_TIMESLOT_REQ_TYPE_NORMAL };

typedef struct {
	uint8_t request_type;
	union {
		struct {
			uint8_t hfclk;
			uint8_t priority;
			uint32_t length_us;
			uint32_t timeout_us;
		} earliest;
	} params;
} mpsl_timeslot_request_t;

typedef struct {
	uint8_t callback_action;
} mpsl_timeslot_signal_return_param_t;

typedef mpsl_timeslot_signal_return_param_t *(*mpsl_timeslot_callback_t)(
	mpsl_timeslot_session_id_t session_id, uint32_t signal);

int32_t mpsl_timeslot_session_open(mpsl_timeslot_callback_t callback,
				   mpsl_timeslot_session_id_t *session_id);
int32_t mpsl_timeslot_request(mpsl_timeslot_session_id_t session_id,
			      const mpsl_timeslot_request_t *request);
