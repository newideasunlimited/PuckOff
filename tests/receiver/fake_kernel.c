/* SPDX-License-Identifier: Apache-2.0 */
#include "test_support.h"
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>

uint64_t test_time_us;
void (*test_clock_hook)(void);
int test_thread_count;
int test_hwinfo_result = 8;
int test_ready_logs;
bool test_led_ready;
int test_led_config_error;
int test_led_write_error;

int64_t k_uptime_ticks(void)
{
	if (test_clock_hook) { test_clock_hook(); }
	return (int64_t)(test_time_us * CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000000U);
}
uint32_t k_uptime_get_32(void) { return (uint32_t)(test_time_us / 1000U); }
void k_msleep(uint32_t ms) { test_time_us += (uint64_t)ms * 1000U; }
void k_usleep(uint32_t us) { test_time_us += us; }
k_tid_t k_thread_create(struct k_thread *thread, void *stack, size_t size,
		       void (*entry)(void *, void *, void *), void *a, void *b, void *c,
		       int priority, uint32_t options, int delay)
{
	(void)stack; (void)size; (void)entry; (void)a; (void)b; (void)c;
	(void)priority; (void)options; (void)delay;
	test_thread_count++;
	return thread;
}
int k_thread_name_set(struct k_thread *thread, const char *name)
{
	(void)thread; (void)name;
	return 0;
}
ssize_t hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
	if (test_hwinfo_result == 8 && length >= 8) {
		memcpy(buffer, "TEST-ID!", 8);
	}
	return test_hwinfo_result;
}
void test_log(const char *format, ...)
{
	if (strcmp(format, "PuckOff ready") == 0) { test_ready_logs++; }
}
int printk(const char *format, ...) { (void)format; return 0; }
bool gpio_is_ready_dt(const struct gpio_dt_spec *gpio)
{
	(void)gpio; return test_led_ready;
}
int gpio_pin_configure_dt(const struct gpio_dt_spec *gpio, int flags)
{
	(void)gpio; (void)flags; return test_led_config_error;
}
int gpio_pin_set_dt(const struct gpio_dt_spec *gpio, int value)
{
	(void)gpio; (void)value; return test_led_write_error;
}
