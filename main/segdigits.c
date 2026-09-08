/* SPDX-License-Identifier: Apache-2.0 */

#include "segdigits.h"

#include "gfx.h"

/* Segment bits: a b c d e f g */
#define SEG_A (1 << 0)
#define SEG_B (1 << 1)
#define SEG_C (1 << 2)
#define SEG_D (1 << 3)
#define SEG_E (1 << 4)
#define SEG_F (1 << 5)
#define SEG_G (1 << 6)

static const uint8_t SEGMENTS[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,          /* 0 */
    SEG_B | SEG_C,                                          /* 1 */
    SEG_A | SEG_B | SEG_G | SEG_E | SEG_D,                  /* 2 */
    SEG_A | SEG_B | SEG_G | SEG_C | SEG_D,                  /* 3 */
    SEG_F | SEG_G | SEG_B | SEG_C,                          /* 4 */
    SEG_A | SEG_F | SEG_G | SEG_C | SEG_D,                  /* 5 */
    SEG_A | SEG_F | SEG_G | SEG_E | SEG_C | SEG_D,          /* 6 */
    SEG_A | SEG_B | SEG_C,                                  /* 7 */
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,  /* 8 */
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,          /* 9 */
};

void seg_digit(uint8_t *buf, int x, int y, int w, int h, int t, int digit, int color)
{
    if (digit < 0 || digit > 9) {
        return;
    }
    uint8_t s = SEGMENTS[digit];

    int mid  = y + (h - t) / 2;         /* top edge of the middle bar */
    int hlen = w - 2 * t;               /* length of a horizontal segment */
    int vtop = mid - (y + t);           /* length of an upper vertical segment */
    int vbot = (y + h - t) - (mid + t); /* length of a lower vertical segment */

    if (s & SEG_A) gfx_fill_rect(buf, x + t,     y,         hlen, t,    color);
    if (s & SEG_G) gfx_fill_rect(buf, x + t,     mid,       hlen, t,    color);
    if (s & SEG_D) gfx_fill_rect(buf, x + t,     y + h - t, hlen, t,    color);
    if (s & SEG_F) gfx_fill_rect(buf, x,         y + t,     t,    vtop, color);
    if (s & SEG_B) gfx_fill_rect(buf, x + w - t, y + t,     t,    vtop, color);
    if (s & SEG_E) gfx_fill_rect(buf, x,         mid + t,   t,    vbot, color);
    if (s & SEG_C) gfx_fill_rect(buf, x + w - t, mid + t,   t,    vbot, color);
}
