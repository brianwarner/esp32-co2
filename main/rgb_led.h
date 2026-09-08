/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    int gpio;
} rgb_led_pins_t;

/* Initializes the onboard WS2812 RGB LED (GPIO 48 on most ESP32-S3 SuperMini
 * boards). Returns an error if the board has no LED there or the RMT
 * peripheral can't be claimed. */
esp_err_t rgb_led_init(const rgb_led_pins_t *pins);

/* Sets the LED to an RGB color (0-255 per channel) and pushes it out. */
esp_err_t rgb_led_set(uint8_t red, uint8_t green, uint8_t blue);
