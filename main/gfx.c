/* SPDX-License-Identifier: Apache-2.0 */

#include "gfx.h"

#include <string.h>

#include "font5x7.h"

void gfx_clear(uint8_t *buf, int color)
{
    memset(buf, color == EPD_WHITE ? 0xFF : 0x00, EPD_BUF_SIZE);
}

void gfx_pixel(uint8_t *buf, int x, int y, int color)
{
    if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H) {
        return;
    }
    uint8_t *p = &buf[y * EPD_ROW_BYTES + (x >> 3)];
    uint8_t mask = 0x80 >> (x & 7);
    if (color == EPD_WHITE) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

void gfx_fill_rect(uint8_t *buf, int x, int y, int w, int h, int color)
{
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            gfx_pixel(buf, xx, yy, color);
        }
    }
}

int gfx_text_width(const char *s, int scale)
{
    int n = (int)strlen(s);
    if (n == 0) {
        return 0;
    }
    return n * (FONT_W + 1) * scale - scale;
}

static void gfx_glyph(uint8_t *buf, int x, int y, char c, int scale, int color)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c < FONT_FIRST || c > FONT_LAST) {
        c = '?';
    }
    const uint8_t *g = &font5x7[(c - FONT_FIRST) * FONT_W];
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (g[col] & (1 << row)) {
                gfx_fill_rect(buf, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void gfx_text(uint8_t *buf, int x, int y, const char *s, int scale, int color)
{
    for (const char *p = s; *p; p++) {
        gfx_glyph(buf, x, y, *p, scale, color);
        x += (FONT_W + 1) * scale;
    }
}

void gfx_text_center(uint8_t *buf, int y, const char *s, int scale, int color)
{
    gfx_text(buf, (EPD_W - gfx_text_width(s, scale)) / 2, y, s, scale, color);
}

void gfx_text_right(uint8_t *buf, int right_x, int y, const char *s, int scale, int color)
{
    gfx_text(buf, right_x - gfx_text_width(s, scale), y, s, scale, color);
}
