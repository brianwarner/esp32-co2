/* SPDX-License-Identifier: Apache-2.0 */
/* SSD1681 driver for the Waveshare 1.54" e-Paper Module V2 (200x200, B/W). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/spi_master.h"
#include "esp_err.h"

#include "gfx.h"   /* EPD_W / EPD_H / EPD_BUF_SIZE and the framebuffer format */

typedef struct {
    spi_host_device_t host;
    int mosi;   /* DIN  */
    int sclk;   /* CLK  */
    int cs;     /* CS   */
    int dc;     /* DC   */
    int rst;    /* RST  */
    int busy;   /* BUSY */
    int clock_hz;
} epd_pins_t;

/* Claims the SPI bus and GPIOs, then leaves the panel in deep sleep. */
esp_err_t epd_init(const epd_pins_t *pins);

/* Both display calls wake the panel, refresh, and return it to deep sleep.
 * Waveshare advise against leaving an e-paper panel powered in a static state. */

/* Full refresh: flashes black/white a few times, clears ghosting. ~2 s. */
void epd_display_full(const uint8_t *buf);

/* Differential refresh against the previously displayed frame, given as prev.
 * ~0.4 s and no flashing, but ghosting accumulates over successive calls, so a
 * full refresh is still needed periodically. */
void epd_display_partial(const uint8_t *buf, const uint8_t *prev);

/* Deep sleep. Called automatically after each refresh. */
void epd_sleep(void);
