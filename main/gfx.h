/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdint.h>

#define EPD_W          200
#define EPD_H          200
#define EPD_ROW_BYTES  (EPD_W / 8)              /* 25   */
#define EPD_BUF_SIZE   (EPD_ROW_BYTES * EPD_H)  /* 5000 */

/* Framebuffer format matches the SSD1681 RAM: 1 bit per pixel, MSB is the
 * leftmost pixel of each byte, bit set = white, bit clear = black. */
#define EPD_BLACK 0
#define EPD_WHITE 1

void gfx_clear(uint8_t *buf, int color);
void gfx_pixel(uint8_t *buf, int x, int y, int color);
void gfx_fill_rect(uint8_t *buf, int x, int y, int w, int h, int color);

/* Glyph cell is 6*scale wide (5 columns plus one spacing column). */
int  gfx_text_width(const char *s, int scale);
void gfx_text(uint8_t *buf, int x, int y, const char *s, int scale, int color);
void gfx_text_center(uint8_t *buf, int y, const char *s, int scale, int color);
void gfx_text_right(uint8_t *buf, int right_x, int y, const char *s, int scale, int color);
