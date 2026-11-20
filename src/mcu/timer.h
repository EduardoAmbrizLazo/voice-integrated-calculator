/* timer.h - the countdown timer
 *
 *   digits      type the time, filling from the right like a microwave:
 *               "one three zero" -> 01:30, "four five" -> 00:45 (minutes:seconds, up to 99:99;
 *               "nine nine" = 99 s, which runs as 01:39)
 *   "start"     start, or carry on after a pause
 *   "stop"      pause
 *   "clear"     stop and go back to 00:00
 *   at zero     TMR_ALARM until any word acknowledges it
 * Digits are ignored while it runs (say "stop" first); after a pause they type a new time.
 * Time comes from the caller in milliseconds, so it keeps counting whatever mode the
 * screen is in. No hardware in here, so it can be tested on a PC (tests/host/modes_test.c).
 */
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

enum { TMR_SET,         /* stopped: digits type a time                         */
       TMR_RUNNING,     /* counting down                                       */
       TMR_PAUSED,      /* stopped part way: "start" carries on                */
       TMR_ALARM };     /* reached zero: waiting for any word                  */

/* what tmr_word did with a word */
enum { TMR_IGNORED = 0, /* not a timer word (or a digit while running)         */
       TMR_USED,        /* acted on it                                         */
       TMR_ACKED };     /* it was the word that stopped the alarm (do nothing else with it) */

void     tmr_reset(void);
int      tmr_word(int w, uint32_t now_ms);
int      tmr_tick(uint32_t now_ms);         /* call often: returns 1 once, when it reaches zero */
int      tmr_state(void);
int      tmr_active(void);                  /* running or paused (worth showing in other modes) */
void     tmr_text(char *out, uint32_t now_ms);   /* "01:30", at least 6 bytes                    */

#endif
