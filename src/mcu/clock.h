/* clock.h - setting the clock by voice (the time itself is kept by the STM32's RTC)
 *
 * Say the time the way you would read it, hours first (24-hour clock):
 *   "one zero three zero"  -> 10:30, set as soon as the 4th digit is heard
 *   "nine three zero"      -> 09:30 (a first digit of 3..9 can only mean 03..09 o'clock)
 *   "two three five nine"  -> 23:59
 * The screen fills in as you go: 1_:__  10:__  10:3_  10:30.
 * Each digit is checked when it is heard, so an impossible time cannot be typed: "two five"
 * is refused at the 5 (hours only go to 23), a minute starting with 6..9 is refused (minutes
 * only go to 59). A refused digit is just dropped: say the right one.
 *   "equals" / "start"     set it early: "one zero equals" -> 10:00, "one three zero equals" -> 01:30
 *   "clear" / "stop"       forget what was typed
 * No hardware in here, so it can be tested on a PC (tests/host/modes_test.c).
 */
#ifndef VIC_CLOCK_H
#define VIC_CLOCK_H

/* what clk_word did with a word */
enum { CLK_IGNORED = 0,     /* not a clock word                                     */
       CLK_TYPING,          /* a digit was added                                    */
       CLK_SET,             /* *h, *m hold the new time: set the RTC                */
       CLK_BAD_HOUR,        /* that digit would make the hour over 23: dropped      */
       CLK_BAD_MIN,         /* that digit would make the minutes over 59: dropped   */
       CLK_CANCEL };        /* what was typed is forgotten                          */

void clk_reset(void);
int  clk_word(int w, int *h, int *m);
int  clk_typing(void);              /* 1 while some digits have been typed              */
void clk_entry(char *out);          /* the time so far, e.g. "10:3_", at least 6 bytes  */

#endif
