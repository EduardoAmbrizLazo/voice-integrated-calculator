/* Self-checking test of src/mcu/modes.c, timer.c and clock.c: mode switching by
 * voice, the countdown timer (with simulated time) and setting the clock.
 * Exit status 1 if anything fails. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "modes.h"
#include "words.h"
#include "calc.h"
#include "timer.h"
#include "clock.h"

static int fails, total;
static uint32_t now;                    /* simulated milliseconds */
static int H, M;                        /* last time handed to the RTC */

static void ok(int cond, const char *what)
{
    total++;
    if (!cond) fails++;
    printf("  %s  %s\n", cond ? "pass" : "FAIL", what);
}

static int say(int w) { return mode_word(w, now, &H, &M); }

static int says(const char *words)      /* digits, + - x = c, k=clock t=timer s=start p=stop */
{
    int r = -1;
    for (const char *p = words; *p; p++) {
        int w = -1;
        if (*p >= '0' && *p <= '9') w = *p - '0';
        else if (*p == '+') w = W_PLUS;    else if (*p == '-') w = W_MINUS;
        else if (*p == 'x') w = W_MULTIPLY; else if (*p == '=') w = W_EQUALS;
        else if (*p == 'c') w = W_CLEAR;   else if (*p == 'k') w = W_CLOCK;
        else if (*p == 't') w = W_TIMER;   else if (*p == 's') w = W_START;
        else if (*p == 'p') w = W_STOP;
        if (w >= 0) r = say(w);
    }
    return r;
}

/* let simulated time pass in 100 ms steps; returns how many times the alarm went off */
static int wait_ms(uint32_t ms)
{
    int alarms = 0;
    for (uint32_t t = 0; t < ms; t += 100) { now += 100; alarms += mode_tick(now); }
    return alarms;
}

static int timer_is(const char *want) { char b[8]; tmr_text(b, now); return !strcmp(b, want); }

int main(void)
{
    char b[16];
    now = 1000;

    printf("switching modes by voice\n");
    mode_reset();
    ok(mode_get() == MODE_CALC, "starts in the calculator");
    ok(says("k") == MW_SWITCHED && mode_get() == MODE_CLOCK, "\"clock\" opens the clock");
    ok(says("k") == MW_SWITCHED && mode_get() == MODE_CALC, "\"clock\" again goes back to the calculator");
    ok(says("t") == MW_SWITCHED && mode_get() == MODE_TIMER, "\"timer\" opens the timer");
    ok(says("t") == MW_SWITCHED && mode_get() == MODE_CALC, "\"timer\" again goes back to the calculator");
    ok((says("k"), says("t"), mode_get() == MODE_TIMER), "\"timer\" from the clock goes to the timer");
    ok((says("k"), mode_get() == MODE_CLOCK), "\"clock\" from the timer goes to the clock");
    mode_reset();
    says("7x");
    says("k"); says("k");
    ok(!strcmp(calc_expr(), "7 X"), "a half-typed sum survives a trip to the clock");
    says("6=");
    ok(!strcmp(calc_display(), "42"), "...and finishes: 7 X 6 = 42");
    ok(says("s") == MW_NOT_HERE, "\"start\" means nothing in the calculator");

    printf("timer\n");
    mode_reset();
    says("t");
    says("130");
    ok(timer_is("01:30"), "\"one three zero\" types 01:30");
    says("s");
    ok(tmr_state() == TMR_RUNNING, "\"start\" starts it");
    wait_ms(10000);
    ok(timer_is("01:20"), "10 s later it shows 01:20");
    ok(says("5") == MW_STOP_FIRST && timer_is("01:20"), "digits are ignored while it runs");
    says("p");
    wait_ms(5000);
    ok(tmr_state() == TMR_PAUSED && timer_is("01:20"), "\"stop\" pauses it: still 01:20 after 5 s");
    says("s");
    wait_ms(1000);
    ok(timer_is("01:19"), "\"start\" carries on: 01:19 after 1 s");
    says("t");
    ok(mode_get() == MODE_CALC && tmr_active(), "back in the calculator, the timer keeps going");
    says("2+2=");
    int alarms = wait_ms(79000);
    ok(alarms == 1 && mode_get() == MODE_TIMER && tmr_state() == TMR_ALARM,
       "at zero the alarm goes off once and the timer screen comes up");
    ok(!strcmp(calc_display(), "4"), "the calculator still shows 4 underneath");
    ok(says("7") == MW_ALARM_OFF && tmr_state() == TMR_SET && timer_is("00:00"),
       "any word (\"seven\") stops the alarm and is not typed");
    ok(wait_ms(5000) == 0, "the alarm does not go off again");
    says("99s");
    ok(timer_is("01:39"), "\"nine nine\" = 99 s, runs as 01:39");
    says("c");
    ok(tmr_state() == TMR_SET && timer_is("00:00"), "\"clear\" stops and resets it");
    says("s");
    ok(tmr_state() == TMR_SET, "\"start\" with nothing typed does nothing");
    says("12345");
    ok(timer_is("23:45"), "more than 4 digits keeps the last 4");
    says("ps");
    ok(tmr_state() == TMR_RUNNING, "start after typing");
    says("p5");
    ok(tmr_state() == TMR_SET && timer_is("00:05"), "a digit after a pause types a new time");
    says("s");
    wait_ms(400);
    ok(timer_is("00:05"), "0.4 s in, 4.6 s left shows 00:05 (rounds up)");
    ok(wait_ms(4700) == 1, "a 5 s timer goes off after 5 s");
    says("c");

    printf("clock\n");
    mode_reset();
    says("k");
    ok(says("930") == MW_CLOCK_TYPING, "digits type a time");
    clk_entry(b);
    ok(!strcmp(b, "SET 09:30"), "\"nine three zero\" shows SET 09:30");
    ok(says("=") == MW_CLOCK_SET && H == 9 && M == 30, "\"equals\" sets the clock to 9:30");
    ok(!clk_typing(), "...and the typing is finished");
    ok((says("2359s"), H == 23 && M == 59), "\"start\" also sets it: 23:59");
    ok(says("2500=") == MW_CLOCK_BAD, "25:00 is refused");
    ok(says("1260=") == MW_CLOCK_BAD, "12:60 is refused");
    ok(says("12c") == MW_CLOCK_CANCEL && !clk_typing(), "\"clear\" forgets a half-typed time");
    ok(says("=") == MW_NOT_HERE, "\"equals\" with nothing typed does nothing");
    says("8");
    says("k");
    says("k");
    ok(!clk_typing(), "leaving the clock forgets a half-typed time");
    ok(says("+") == MW_NOT_HERE, "\"plus\" means nothing on the clock");

    printf("\n%s: %d of %d mode/timer/clock checks passed\n", fails ? "FAIL" : "PASS", total - fails, total);
    return fails ? 1 : 0;
}
