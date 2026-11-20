/* modes.h - which mode the device is in, and who gets each recognised word
 *
 *   CALCULATOR (start-up) / CLOCK / TIMER
 *   "clock" opens the clock; saying "clock" again goes back to the calculator. Same for "timer".
 *   Going to another mode loses nothing: a sum half typed is still there when you come back,
 *   and the timer keeps counting (the screen shows it on the top row).
 *   When the timer reaches zero the device switches to the timer, whatever mode it was in,
 *   and the next word (any word) only stops the alarm.
 *
 * No hardware in here, so it can be tested on a PC (tests/host/modes_test.c).
 */
#ifndef MODES_H
#define MODES_H

#include <stdint.h>

enum { MODE_CALC, MODE_CLOCK, MODE_TIMER };

/* what mode_word did, so the screen can say so */
enum { MW_CALC,         /* the calculator used it                                  */
       MW_SWITCHED,     /* "clock"/"timer": the mode changed                       */
       MW_ALARM_OFF,    /* it stopped the timer's alarm                            */
       MW_CLOCK_TYPING, /* a digit of a new time                                   */
       MW_CLOCK_SET,    /* set the RTC to *h:*m                                    */
       MW_CLOCK_BAD,    /* not a real time                                         */
       MW_CLOCK_CANCEL, /* typed time forgotten                                    */
       MW_TIMER,        /* the timer used it                                       */
       MW_STOP_FIRST,   /* a digit while the timer runs                            */
       MW_NOT_HERE };   /* means nothing in this mode (e.g. "start" in the calculator) */

void mode_reset(void);                      /* calculator mode, everything cleared */
int  mode_get(void);
int  mode_word(int w, uint32_t now_ms, int *h, int *m);
int  mode_tick(uint32_t now_ms);            /* call often: returns 1 when the timer goes off
                                               (the mode is now MODE_TIMER)                 */

#endif
