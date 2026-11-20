/* clock.c - see clock.h */
#include "clock.h"
#include "words.h"

static int typed;                       /* HHMM as a number 0..9999 */
static int ndigits;

void clk_reset(void)  { typed = 0; ndigits = 0; }
int  clk_typing(void) { return ndigits > 0; }

int clk_word(int w, int *h, int *m)
{
    if (w >= 0 && w <= 9) {
        typed = (typed * 10 + w) % 10000;
        ndigits++;
        return CLK_TYPING;
    }
    if (w == W_EQUALS || w == W_START) {
        if (!ndigits) return CLK_IGNORED;
        int hh = typed / 100, mm = typed % 100;
        clk_reset();
        if (hh > 23 || mm > 59) return CLK_BAD;
        *h = hh; *m = mm;
        return CLK_SET;
    }
    if (w == W_CLEAR || w == W_STOP) {
        if (!ndigits) return CLK_IGNORED;
        clk_reset();
        return CLK_CANCEL;
    }
    return CLK_IGNORED;
}

void clk_entry(char *out)
{
    const char *s = "SET ";
    while (*s) *out++ = *s++;
    out[0] = (char)('0' + typed / 1000); out[1] = (char)('0' + typed / 100 % 10); out[2] = ':';
    out[3] = (char)('0' + typed / 10 % 10); out[4] = (char)('0' + typed % 10); out[5] = 0;
}
