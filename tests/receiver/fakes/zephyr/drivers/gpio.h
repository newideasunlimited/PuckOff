/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
struct gpio_dt_spec { int pin; };
#define GPIO_DT_SPEC_GET(...) {0}
#define GPIO_OUTPUT_INACTIVE 0
bool gpio_is_ready_dt(const struct gpio_dt_spec *gpio);
int gpio_pin_configure_dt(const struct gpio_dt_spec *gpio, int flags);
int gpio_pin_set_dt(const struct gpio_dt_spec *gpio, int value);
