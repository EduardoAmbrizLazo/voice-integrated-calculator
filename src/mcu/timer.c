/* timer.c - see timer.h */
#include "timer.h"
#include "words.h"

static int      st;
static uint32_t typed;                  /* digits typed, MMSS as a number 0..9999 */
static uint32_t left_ms;                /* time left when paused                  */
static uint32_t end_ms;                 /* when it reaches zero, while running    */

void tmr_reset(void) { st = TMR_SET; typed = 0; left_ms = 0; }

int tmr_state(void)  { return st; }
int tmr_active(void) { return st == TMR_RUNNING || st == TMR_PAUSED; }

int tmr_word(int w, uint32_t now)
{
    if (st == TMR_ALARM) { tmr_reset(); return TMR_ACKED; }     /* any word stops the alarm */

    if (w >= 0 && w <= 9) {
        if (st == TMR_RUNNING) return TMR_IGNORED;              /* say "stop" first */
        if (st == TMR_PAUSED) tmr_reset();                      /* digits after a pause: a new time */
        typed = (typed * 10 + (uint32_t)w) % 10000;
        return TMR_USED;
    }
    switch (w) {
    case W_START:
        if (st == TMR_PAUSED) { end_ms = now + left_ms; st = TMR_RUNNING; }
        else if (st == TMR_SET) {
            uint32_t secs = (typed / 100) * 60 + typed % 100;
            if (secs > TMR_MAX_S) secs = TMR_MAX_S;             /* 99:99 would be 100:39: the screen has 2 minute digits */
            if (secs) { end_ms = now + secs * 1000u; st = TMR_RUNNING; }
        }
        return TMR_USED;
    case W_STOP:
        if (st == TMR_RUNNING) {
            int32_t d = (int32_t)(end_ms - now);
            left_ms = d > 0 ? (uint32_t)d : 0;
            st = TMR_PAUSED;
        }
        return TMR_USED;
    case W_CLEAR:
        tmr_reset();
        return TMR_USED;
    default:
        return TMR_IGNORED;
    }
}

int tmr_tick(uint32_t now)
{
    if (st == TMR_RUNNING && (int32_t)(now - end_ms) >= 0) {
        st = TMR_ALARM;
        typed = 0; left_ms = 0;
        return 1;
    }
    return 0;
}

void tmr_text(char *out, uint32_t now)
{
    uint32_t mm, ss;
    if (st == TMR_SET) { mm = typed / 100; ss = typed % 100; }  /* as typed, e.g. 00:99 */
    else {
        uint32_t ms = 0;
        if (st == TMR_RUNNING) { int32_t d = (int32_t)(end_ms - now); ms = d > 0 ? (uint32_t)d : 0; }
        else if (st == TMR_PAUSED) ms = left_ms;
        uint32_t secs = (ms + 999) / 1000;                      /* round up: 0.4 s left shows 00:01 */
        mm = secs / 60; ss = secs % 60;
    }
    out[0] = (char)('0' + mm / 10 % 10); out[1] = (char)('0' + mm % 10); out[2] = ':';
    out[3] = (char)('0' + ss / 10);      out[4] = (char)('0' + ss % 10); out[5] = 0;
}
