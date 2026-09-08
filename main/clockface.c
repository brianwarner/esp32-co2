/* SPDX-License-Identifier: Apache-2.0 */

#include "clockface.h"

#include <stdio.h>
#include <string.h>

#include "env_config.h"
#include "gfx.h"
#include "segdigits.h"

/* ---- Layout (200x200) ---------------------------------------------------
 * Top half: current readings. Bottom half: 72h CO2 history.
 */
#define MARGIN 8

/* -- Top half: readings -- */
#define Y_HEADER      6
#define Y_RULE_TOP    20
#define Y_CO2         26
#define CO2_DIGIT_W   34
#define CO2_DIGIT_H   52
#define CO2_DIGIT_T   8
#define CO2_DIGIT_GAP 5
#define CO2_DIGITS    4
#define Y_UNIT_TAG    68
#define Y_RULE_MID    80
#define Y_STATS       86

/* -- Bottom half: histogram -- */
#define PLOT_W       CO2_HISTORY_LEN /* one column per sample, one px wide */
#define PLOT_X0      (200 - MARGIN - PLOT_W)
#define PLOT_Y_TOP   108
#define PLOT_Y_BASE  182
#define PLOT_H       (PLOT_Y_BASE - PLOT_Y_TOP)
#define Y_AXIS_LABEL 186

/* CO2 never legitimately reads 0; used as the "no data yet" sentinel. */
#define CO2_FLOOR_PPM 400

static void draw_rule(uint8_t *buf, int y)
{
    gfx_fill_rect(buf, MARGIN, y, 200 - 2 * MARGIN, 2, EPD_BLACK);
}

/* Draws up to `digits` seven-segment digits of `value`, blanking leading
 * zeroes beyond the first (so 482 doesn't render as 0482), and returns the
 * x just past the last digit drawn. */
static int draw_number(uint8_t *buf, int x, int y, int digits, int value)
{
    int div = 1;
    for (int i = 1; i < digits; i++) {
        div *= 10;
    }

    bool leading = true;
    for (int i = 0; i < digits; i++) {
        int d = (value / div) % 10;
        bool last = (i == digits - 1);
        if (d == 0 && leading && !last) {
            /* blank, but still advance so the number stays right-aligned */
        } else {
            leading = false;
            seg_digit(buf, x, y, CO2_DIGIT_W, CO2_DIGIT_H, CO2_DIGIT_T, d, EPD_BLACK);
        }
        x += CO2_DIGIT_W + CO2_DIGIT_GAP;
        div /= 10;
    }
    return x - CO2_DIGIT_GAP;
}

static void draw_readings(uint8_t *buf, const struct tm *tm, bool time_valid,
                          const co2_reading_t *reading)
{
    char line[32];

    gfx_text(buf, MARGIN, Y_HEADER, "CO2", 2, EPD_BLACK);

    if (!time_valid) {
        snprintf(line, sizeof(line), "--:--");
    } else if (CFG_CLOCK_24H) {
        snprintf(line, sizeof(line), "%02d:%02d", tm->tm_hour, tm->tm_min);
    } else {
        int h12 = tm->tm_hour % 12;
        if (h12 == 0) {
            h12 = 12;
        }
        snprintf(line, sizeof(line), "%d:%02d %s", h12, tm->tm_min, tm->tm_hour >= 12 ? "PM" : "AM");
    }
    gfx_text_right(buf, 200 - MARGIN, Y_HEADER, line, 2, EPD_BLACK);

    draw_rule(buf, Y_RULE_TOP);

    if (!reading->valid) {
        gfx_text_center(buf, Y_CO2 + (CO2_DIGIT_H - 14) / 2, "NO SENSOR", 2, EPD_BLACK);
    } else {
        int value = reading->co2_ppm;
        if (value > 9999) {
            value = 9999;
        }
        int block_w = CO2_DIGITS * CO2_DIGIT_W + (CO2_DIGITS - 1) * CO2_DIGIT_GAP;
        int x = (200 - block_w) / 2;
        draw_number(buf, x, Y_CO2, CO2_DIGITS, value);
        gfx_text_right(buf, 200 - MARGIN, Y_UNIT_TAG, "PPM", 1, EPD_BLACK);
    }

    draw_rule(buf, Y_RULE_MID);

    if (!reading->valid) {
        gfx_text(buf, MARGIN, Y_STATS, "--", 2, EPD_BLACK);
        gfx_text_right(buf, 200 - MARGIN, Y_STATS, "--%", 2, EPD_BLACK);
        return;
    }

    float t = reading->temperature_c;
    if (CFG_TEMP_UNIT_F) {
        t = t * 9.0f / 5.0f + 32.0f;
    }
    snprintf(line, sizeof(line), "%d%s", (int)(t + (t >= 0 ? 0.5f : -0.5f)),
             CFG_TEMP_UNIT_F ? "F" : "C");
    gfx_text(buf, MARGIN, Y_STATS, line, 2, EPD_BLACK);

    snprintf(line, sizeof(line), "%d%%RH", (int)(reading->humidity_pct + 0.5f));
    gfx_text_right(buf, 200 - MARGIN, Y_STATS, line, 2, EPD_BLACK);
}

/* Smallest "nice" ceiling that keeps the highest bar off the top rule. */
static uint16_t histogram_ceiling(uint16_t peak)
{
    static const uint16_t CEILINGS[] = {1000, 1500, 2000, 3000, 4000, 5000};
    for (size_t i = 0; i < sizeof(CEILINGS) / sizeof(CEILINGS[0]); i++) {
        if ((uint32_t)peak * 10 <= (uint32_t)CEILINGS[i] * 9) {
            return CEILINGS[i];
        }
    }
    return CEILINGS[sizeof(CEILINGS) / sizeof(CEILINGS[0]) - 1];
}

static void draw_range_label(uint8_t *buf, int y, uint16_t ppm)
{
    char line[8];
    snprintf(line, sizeof(line), "%u", ppm);
    gfx_text_right(buf, PLOT_X0 - 6, y, line, 1, EPD_BLACK);
}

static void draw_histogram(uint8_t *buf, const uint16_t history[CO2_HISTORY_LEN])
{
    uint16_t peak = CO2_FLOOR_PPM;
    for (int i = 0; i < CO2_HISTORY_LEN; i++) {
        if (history[i] > peak) {
            peak = history[i];
        }
    }
    uint16_t ceiling = histogram_ceiling(peak);
    uint16_t floor = CO2_FLOOR_PPM;

    draw_range_label(buf, PLOT_Y_TOP - 4, ceiling);
    draw_range_label(buf, (PLOT_Y_TOP + PLOT_Y_BASE) / 2 - 4, (ceiling + floor) / 2);
    draw_range_label(buf, PLOT_Y_BASE - 7, floor);

    for (int i = 0; i < CO2_HISTORY_LEN; i++) {
        uint16_t value = history[i];
        if (value == 0) {
            continue; /* no reading recorded for this bucket yet */
        }
        if (value < floor) {
            value = floor;
        }
        if (value > ceiling) {
            value = ceiling;
        }
        int h = (int)((uint32_t)(value - floor) * PLOT_H / (ceiling - floor));
        if (h < 1) {
            h = 1;
        }
        gfx_fill_rect(buf, PLOT_X0 + i, PLOT_Y_BASE - h, 1, h, EPD_BLACK);
    }

    gfx_fill_rect(buf, PLOT_X0, PLOT_Y_BASE, PLOT_W, 1, EPD_BLACK);

    gfx_text(buf, PLOT_X0, Y_AXIS_LABEL, "72H", 1, EPD_BLACK);
    gfx_text(buf, PLOT_X0 + PLOT_W / 3 - gfx_text_width("48H", 1) / 2, Y_AXIS_LABEL, "48H", 1,
             EPD_BLACK);
    gfx_text(buf, PLOT_X0 + 2 * PLOT_W / 3 - gfx_text_width("24H", 1) / 2, Y_AXIS_LABEL, "24H", 1,
             EPD_BLACK);
    gfx_text_right(buf, PLOT_X0 + PLOT_W, Y_AXIS_LABEL, "NOW", 1, EPD_BLACK);
}

void clockface_render(uint8_t *buf, const struct tm *tm, bool time_valid,
                      const co2_reading_t *reading, const uint16_t history[CO2_HISTORY_LEN])
{
    gfx_clear(buf, EPD_WHITE);
    draw_readings(buf, tm, time_valid, reading);
    draw_histogram(buf, history);
}

void clockface_status(uint8_t *buf, const char *title, const char *detail)
{
    gfx_clear(buf, EPD_WHITE);
    gfx_text_center(buf, 82, title, 3, EPD_BLACK);
    if (detail && *detail) {
        gfx_text_center(buf, 116, detail, 2, EPD_BLACK);
    }
}
