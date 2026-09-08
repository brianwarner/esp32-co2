/* SPDX-License-Identifier: Apache-2.0 */
/* Driver for the onboard WS2812 RGB LED, via the espressif/led_strip
 * managed component (RMT backend). */

#include "rgb_led.h"

#include "led_strip.h"

static led_strip_handle_t s_strip;

esp_err_t rgb_led_init(const rgb_led_pins_t *pins)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = pins->gpio,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };

    return led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
}

esp_err_t rgb_led_set(uint8_t red, uint8_t green, uint8_t blue)
{
    esp_err_t err = led_strip_set_pixel(s_strip, 0, red, green, blue);
    if (err != ESP_OK)
    {
        return err;
    }
    return led_strip_refresh(s_strip);
}
