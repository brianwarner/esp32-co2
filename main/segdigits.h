/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/* Seven-segment digits drawn as filled rectangles, so the size is free.
 * w = digit width, h = digit height, t = stroke thickness.
 * digit < 0 draws nothing (used to blank a leading zero). */
void seg_digit(uint8_t *buf, int x, int y, int w, int h, int t, int digit, int color);
