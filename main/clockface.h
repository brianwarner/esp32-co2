/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* 72 hours of history at 30-minute resolution, one bar per sample. */
#define CO2_HISTORY_LEN 144

typedef struct {
    bool valid; /* false until the first good sensor reading arrives */
    uint16_t co2_ppm;
    float temperature_c;
    float humidity_pct;
} co2_reading_t;

/* Draws into a 200x200 framebuffer. No hardware dependencies, so the layout can
 * be compiled and previewed on the host (see tools/preview.c).
 *
 * history holds the max CO2 reading (ppm) seen in each 30-minute bucket over
 * the last 72 hours, oldest first; history[CO2_HISTORY_LEN - 1] is the
 * current, still-filling bucket. A bucket with no reading yet is 0.
 *
 * tm is ignored (may be NULL) when time_valid is false, which draws a
 * placeholder in place of the clock - for use before a real time source
 * (NTP, or in the future an RTC) has been established. */
void clockface_render(uint8_t *buf, const struct tm *tm, bool time_valid,
                      const co2_reading_t *reading, const uint16_t history[CO2_HISTORY_LEN]);
void clockface_status(uint8_t *buf, const char *title, const char *detail);
