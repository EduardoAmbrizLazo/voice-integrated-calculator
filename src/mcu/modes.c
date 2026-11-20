/* modes.c - see modes.h */
#include "modes.h"
#include "words.h"
#include "calc.h"
#include "clock.h"
#include "timer.h"

static int mode;

void mode_reset(void)
{
    mode = MODE_CALC;
    calc_reset();
    clk_reset();
    tmr_reset();
}

int mode_get(void) { return mode; }

static void go(int m)
{
    if (mode == MODE_CLOCK) clk_reset();        /* leaving the clock: forget a half-typed time */
    mode = m;
}

int mode_word(int w, uint32_t now, int *h, int *m)
{
    if (tmr_state() == TMR_ALARM) {             /* any word stops the alarm, and nothing else */
        tmr_word(w, now);
        mode = MODE_TIMER;
        return MW_ALARM_OFF;
    }
    if (w == W_CLOCK) { go(mode == MODE_CLOCK ? MODE_CALC : MODE_CLOCK); return MW_SWITCHED; }
    if (w == W_TIMER) { go(mode == MODE_TIMER ? MODE_CALC : MODE_TIMER); return MW_SWITCHED; }

    switch (mode) {
    case MODE_CLOCK:
        switch (clk_word(w, h, m)) {
        case CLK_TYPING: return MW_CLOCK_TYPING;
        case CLK_SET:    return MW_CLOCK_SET;
        case CLK_BAD_HOUR: return MW_CLOCK_BAD_HOUR;
        case CLK_BAD_MIN:  return MW_CLOCK_BAD_MIN;
        case CLK_CANCEL: return MW_CLOCK_CANCEL;
        default:         return MW_NOT_HERE;
        }
    case MODE_TIMER:
        if (tmr_word(w, now) == TMR_USED) return MW_TIMER;
        return (w >= 0 && w <= 9) ? MW_STOP_FIRST : MW_NOT_HERE;
    default:
        return calc_word(w) ? MW_CALC : MW_NOT_HERE;
    }
}

int mode_tick(uint32_t now)
{
    if (!tmr_tick(now)) return 0;
    go(MODE_TIMER);
    return 1;
}
