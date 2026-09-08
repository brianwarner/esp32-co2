/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/* 5x7 glyphs for ASCII 0x20..0x5F, 5 column bytes each, bit 0 = top row.
 * Lowercase input is folded to uppercase by the renderer. */
#define FONT_FIRST 0x20
#define FONT_LAST  0x5F
#define FONT_W     5
#define FONT_H     7

extern const uint8_t font5x7[(FONT_LAST - FONT_FIRST + 1) * FONT_W];
