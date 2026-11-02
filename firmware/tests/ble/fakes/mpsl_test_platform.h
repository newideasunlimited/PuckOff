/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_PUCKOFF_TRANSPORT_BLE_RF 1
#define CONFIG_SOC_NRF52840 1
#define CONFIG_BT_LL_SOFTDEVICE 1
#define CONFIG_MPSL_TIMESLOT_SESSION_COUNT 1
#define CONFIG_NRF_RTC_TIMER 1
#define CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC 32768
#define CONFIG_SYS_CLOCK_TICKS_PER_SEC 32768
#define CONFIG_NRF_RTC_COUNTER_BIT_WIDTH 24
#define CONFIG_MPSL_LOW_PRIO_IRQN 25
#define CONFIG_NRFX_EGU1 0
#define IS_ENABLED(x) (x)
#define BIT(x) (1U << (x))
#define BUILD_ASSERT(condition, ...) _Static_assert((condition), #condition)
#define DT_NODELABEL(x) 0
#define DT_NODE_HAS_STATUS_OKAY(x) 0
#define __aligned(n) __attribute__((aligned(n)))
#define ARG_UNUSED(x) (void)(x)

typedef uint32_t k_timeout_t;
#define K_NO_WAIT 0U
#define K_MSEC(x) ((uint32_t)(x) * 1000U)
#define K_USEC(x) ((uint32_t)(x))
struct k_mutex { bool held; };
struct k_sem { unsigned count; };
#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_SEM_DEFINE(name, initial, limit) struct k_sem name = { initial }
int k_mutex_lock(struct k_mutex *mutex, k_timeout_t timeout);
void k_mutex_unlock(struct k_mutex *mutex);
int k_sem_take(struct k_sem *sem, k_timeout_t timeout);
void k_sem_give(struct k_sem *sem);
void k_sem_reset(struct k_sem *sem);
int64_t k_uptime_ticks(void);
uint64_t k_ticks_to_us_floor64(uint64_t ticks);
void irq_enable(unsigned irq);
void NVIC_SetPendingIRQ(unsigned irq);
void NVIC_ClearPendingIRQ(unsigned irq);
extern void (*fake_completion_irq)(const void *);
#define IRQ_CONNECT(irq, priority, handler, arg, flags) (fake_completion_irq = handler)

void fake_log(const char *format, ...);
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
#define LOG_ERR(...) fake_log(__VA_ARGS__)
#define LOG_WRN(...) fake_log(__VA_ARGS__)
#define LOG_ERR_RATELIMIT(...) fake_log(__VA_ARGS__)
struct shell { int unused; };
void shell_print(const struct shell *shell, const char *format, ...);
#define SHELL_CMD_REGISTER(name, subcmd, help, handler) \
	static int (*const fake_shell_##name)(const struct shell *, size_t, char **) \
		__attribute__((unused)) = handler

typedef struct {
	volatile uint32_t POWER, INTENCLR, INTENSET, SHORTS, STATE;
	volatile uint32_t EVENTS_DISABLED, EVENTS_END, EVENTS_READY, EVENTS_ADDRESS, EVENTS_RSSIEND;
	volatile uint32_t TASKS_DISABLE, TASKS_TXEN, TASKS_RXEN;
	volatile uint32_t MODE, MODECNF0, TXPOWER, FREQUENCY, PCNF0, PCNF1;
	volatile uint32_t CRCCNF, CRCPOLY, CRCINIT, BASE0, PREFIX0;
	volatile uint32_t TXADDRESS, RXADDRESSES, PACKETPTR, CRCSTATUS, RSSISAMPLE;
} NRF_RADIO_Type;
typedef struct {
	volatile uint32_t TASKS_CAPTURE[4], CC[4], EVENTS_COMPARE[4];
	volatile uint32_t INTENCLR, INTENSET;
} NRF_TIMER_Type;
typedef struct { volatile uint32_t COUNTER; } NRF_RTC_Type;
typedef struct {
	volatile uint32_t CHENCLR, CHENSET;
	struct { volatile uint32_t EEP, TEP; } CH[20];
	struct { volatile uint32_t TEP; } FORK[20];
} NRF_PPI_Type;
extern NRF_RADIO_Type fake_radio;
extern NRF_TIMER_Type fake_timer;
extern NRF_RTC_Type fake_rtc;
extern NRF_PPI_Type fake_ppi;
#define NRF_RADIO (&fake_radio)
#define NRF_RTC1 (&fake_rtc)
#define NRF_PPI (&fake_ppi)
#define MPSL_TIMER0 (&fake_timer)
#define SWI1_EGU1_IRQn 21
#define MPSL_PPI_CHANNELS_USED_MASK 0xc0080000U
#define SDC_PPI_CHANNELS_USED_MASK 0xfffe0000U
#define RADIO_STATE_STATE_Disabled 0
#define RADIO_MODE_MODE_Ble_2Mbit 4
#define RADIO_MODE_MODE_Pos 0
#define RADIO_MODECNF0_RU_Fast 1
#define RADIO_MODECNF0_RU_Pos 0
#define RADIO_TXPOWER_TXPOWER_Pos8dBm 8
#define RADIO_TXPOWER_TXPOWER_Pos 0
#define RADIO_PCNF0_LFLEN_Pos 0
#define RADIO_PCNF0_S1LEN_Pos 16
#define RADIO_PCNF1_ENDIAN_Big 1
#define RADIO_PCNF1_ENDIAN_Pos 24
#define RADIO_PCNF1_BALEN_Pos 16
#define RADIO_PCNF1_MAXLEN_Pos 0
#define RADIO_CRCCNF_LEN_Two 2
#define RADIO_CRCCNF_LEN_Pos 0
#define RADIO_INTENSET_DISABLED_Msk BIT(4)
#define RADIO_SHORTS_READY_START_Msk BIT(0)
#define RADIO_SHORTS_END_DISABLE_Msk BIT(1)
#define RADIO_SHORTS_ADDRESS_RSSISTART_Msk BIT(4)
#define RADIO_SHORTS_DISABLED_RSSISTOP_Msk BIT(8)
#define TIMER_INTENSET_COMPARE0_Msk BIT(16)
#define TIMER_INTENSET_COMPARE1_Msk BIT(17)
#define TIMER_INTENSET_COMPARE2_Msk BIT(18)

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
#define MPSL_TIMESLOT_REQ_TYPE_EARLIEST 0
#define MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED 0
#define MPSL_TIMESLOT_PRIORITY_NORMAL 1
#define NRF_EAGAIN 11
#define NRF_ENOMEM 12
#define NRF_EINVAL 22
typedef uint8_t mpsl_timeslot_session_id_t;
typedef struct {
	uint8_t request_type;
	union {
		struct {
			uint8_t hfclk, priority;
			uint32_t length_us, timeout_us;
		} earliest;
	} params;
} mpsl_timeslot_request_t;
typedef struct { uint8_t callback_action; } mpsl_timeslot_signal_return_param_t;
typedef mpsl_timeslot_signal_return_param_t *(*mpsl_timeslot_callback_t)(
	mpsl_timeslot_session_id_t id, uint32_t signal);
int mpsl_timeslot_session_open(mpsl_timeslot_callback_t callback,
			      mpsl_timeslot_session_id_t *id);
int mpsl_timeslot_request(mpsl_timeslot_session_id_t id, const mpsl_timeslot_request_t *request);
int multithreading_lock_acquire(k_timeout_t timeout);
void multithreading_lock_release(void);
