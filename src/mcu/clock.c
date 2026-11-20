/* clock.c - see clock.h */
#include "clock.h"
#include "words.h"

static int d[4];                        /* digits typed, in order: H H M M */
static int nd;                          /* how many                        */
static int padded;                      /* first digit was 3..9, so a 0 was put in front of it */

void clk_reset(void)  { nd = 0; padded = 0; }
int  clk_typing(void) { return nd > 0; }

static int finish(int hh, int mm, int *h, int *m)
{
    clk_reset();
    if (hh > 23) return CLK_BAD_HOUR;
    if (mm > 59) return CLK_BAD_MIN;
    *h = hh; *m = mm;
    return CLK_SET;
}

int clk_word(int w, int *h, int *m)
{
    if (w >= 0 && w <= 9) {
        switch (nd) {
        case 0:
            if (w >= 3) { d[0] = 0; d[1] = w; nd = 2; padded = 1; }     /* "nine" = 09 */
            else { d[0] = w; nd = 1; }
            return CLK_TYPING;
        case 1:
            if (d[0] == 2 && w > 3) return CLK_BAD_HOUR;                 /* 24..29 */
            d[1] = w; nd = 2;
            return CLK_TYPING;
        case 2:
            if (w > 5) return CLK_BAD_MIN;                               /* 60..99 */
            d[2] = w; nd = 3;
            return CLK_TYPING;
        default:
            return finish(d[0] * 10 + d[1], d[2] * 10 + w, h, m);       /* 4th digit: set it */
        }
    }
    if (w == W_EQUALS || w == W_START) {                                /* set what there is */
        switch (nd) {
        case 0:  return CLK_IGNORED;
        case 1:  return finish(d[0], 0, h, m);                          /* "two" = 02:00 */
        case 2:  return finish(d[0] * 10 + d[1], 0, h, m);              /* "one zero" = 10:00 */
        default:
            if (!padded && d[1] <= 5)                                   /* "one three zero" = 1:30 */
                return finish(d[0], d[1] * 10 + d[2], h, m);
            return finish(d[0] * 10 + d[1], d[2] * 10, h, m);           /* "nine three" = 09:30 */
        }
    }
    if (w == W_CLEAR || w == W_STOP) {
        if (!nd) return CLK_IGNORED;
        clk_reset();
        return CLK_CANCEL;
    }
    return CLK_IGNORED;
}

void clk_entry(char *out)
{
    for (int i = 0, k = 0; i < 5; i++) {
        if (i == 2) { out[i] = ':'; continue; }
        out[i] = k < nd ? (char)('0' + d[k]) : '_';
        k++;
    }
    out[5] = 0;
}
