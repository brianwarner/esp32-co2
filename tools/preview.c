/* SPDX-License-Identifier: Apache-2.0 */
/* Renders the clock face on the host and writes a 1-bit BMP, so the layout can
 * be checked without flashing hardware.
 *
 *   make preview
 *   make preview ARGS='"2026-08-26 21:07"'
 *   make preview ARGS='"2026-08-26 21:07" out.bmp'
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clockface.h"
#include "gfx.h"

/* Synthetic 72h CO2 history for the preview: a daily breathing-driven
 * rhythm (low overnight, rising through the day) plus a couple of
 * ventilation dips, so the histogram has something to show. */
static void demo_history(uint16_t out[CO2_HISTORY_LEN])
{
    for (int i = 0; i < CO2_HISTORY_LEN; i++) {
        double hours_ago = (CO2_HISTORY_LEN - 1 - i) * 0.5;
        double hour_of_day = fmod(24.0 - fmod(hours_ago, 24.0), 24.0);
        double daily = 500.0 + 700.0 * (0.5 - 0.5 * cos((hour_of_day / 24.0) * 2 * M_PI));
        double wobble = 80.0 * sin(i * 0.9);
        double value = daily + wobble;
        if (i % 37 == 0) {
            value += 350.0; /* the odd stuffy-room spike */
        }
        if (value < 420.0) {
            value = 420.0;
        }
        out[i] = (uint16_t)value;
    }
}

#define BMP_ROW_PAD ((EPD_ROW_BYTES + 3) & ~3)   /* BMP rows are 4-byte aligned */

static void put16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

static int write_bmp(const char *path, const uint8_t *buf)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return -1;
    }
    uint32_t pixels = (uint32_t)BMP_ROW_PAD * EPD_H;
    uint32_t offset = 14 + 40 + 8;

    fputc('B', f); fputc('M', f);
    put32(f, offset + pixels);
    put16(f, 0); put16(f, 0);
    put32(f, offset);

    put32(f, 40);
    put32(f, EPD_W);
    put32(f, (uint32_t)(-EPD_H));   /* negative height = top-down rows */
    put16(f, 1);
    put16(f, 1);                    /* 1 bit per pixel */
    put32(f, 0);
    put32(f, pixels);
    put32(f, 2835); put32(f, 2835);
    put32(f, 2); put32(f, 2);

    /* Palette: index 0 = black, index 1 = white, matching the framebuffer. */
    fputc(0x00, f); fputc(0x00, f); fputc(0x00, f); fputc(0x00, f);
    fputc(0xFF, f); fputc(0xFF, f); fputc(0xFF, f); fputc(0x00, f);

    for (int y = 0; y < EPD_H; y++) {
        fwrite(&buf[y * EPD_ROW_BYTES], 1, EPD_ROW_BYTES, f);
        for (int p = EPD_ROW_BYTES; p < BMP_ROW_PAD; p++) {
            fputc(0xFF, f);
        }
    }
    fclose(f);
    return 0;
}

static void write_ascii(const uint8_t *buf)
{
    for (int y = 0; y < EPD_H; y += 4) {
        for (int x = 0; x < EPD_W; x += 2) {
            int black = !(buf[y * EPD_ROW_BYTES + (x >> 3)] & (0x80 >> (x & 7)));
            fputc(black ? '#' : '.', stdout);
        }
        fputc('\n', stdout);
    }
}

int main(int argc, char **argv)
{
    static uint8_t buf[EPD_BUF_SIZE];

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    if (argc > 1) {
        memset(&tm, 0, sizeof(tm));
        tm.tm_isdst = -1;
        if (!strptime(argv[1], "%Y-%m-%d %H:%M", &tm)) {
            fprintf(stderr, "usage: %s [\"YYYY-MM-DD HH:MM\"] [out.bmp]\n", argv[0]);
            return 1;
        }
        time_t t = mktime(&tm);
        localtime_r(&t, &tm);
    }

    co2_reading_t reading = {
        .valid = true,
        .co2_ppm = 812,
        .temperature_c = 22.5f,
        .humidity_pct = 41.0f,
    };
    uint16_t history[CO2_HISTORY_LEN];
    demo_history(history);

    clockface_render(buf, &tm, true, &reading, history);
    write_ascii(buf);

    const char *out = argc > 2 ? argv[2] : "preview.bmp";
    if (write_bmp(out, buf) != 0) {
        return 1;
    }
    fprintf(stderr, "wrote %s\n", out);
    return 0;
}
