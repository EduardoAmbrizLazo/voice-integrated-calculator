/* clock.h - setting the clock by voice (the time itself is kept by the STM32's RTC)
 *
 *   digits             type a time, filling from the right: "nine three zero" -> 09:30,
 *                      "one two four five" -> 12:45 (24-hour)
 *   "equals"/"start"   set the clock to it (if it is a real time)
 *   "clear"/"stop"     forget what was typed
 * No hardware in here, so it can be tested on a PC (tests/host/modes_test.c).
 */
#ifndef VIC_CLOCK_H
#define VIC_CLOCK_H

/* what clk_word did with a word */
enum { CLK_IGNORED = 0,     /* not a clock word                            */
       CLK_TYPING,          /* a digit was added                           */
       CLK_SET,             /* *h, *m hold the new time: set the RTC       */
       CLK_BAD,             /* typed something that is not a time (25:00)  */
       CLK_CANCEL };        /* what was typed is forgotten                 */

void clk_reset(void);
int  clk_word(int w, int *h, int *m);
int  clk_typing(void);              /* 1 while digits have been typed           */
void clk_entry(char *out);          /* "SET 09:30" while typing, at least 10 bytes */

#endif
