# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS "main.c"
         "epd_ssd1681.c"
         "gfx.c"
         "font5x7.c"
         "segdigits.c"
         "clockface.c"
         "net_time.c"
         "co2_stcc4.c"
         "rgb_led.c"
         "co2_history_store.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi esp_netif esp_event nvs_flash lwip led_strip esp_timer)
