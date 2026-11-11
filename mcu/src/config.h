/* Tunable parameters for the voice calculator.  Everything a team is likely to
 * adjust on the bench lives here. */
#ifndef CONFIG_H
#define CONFIG_H

/* ---- vocabulary ---- */
#define VOCAB_SIZE        19
#define TEMPL_PER_WORD    3
#define NUM_TEMPL         (VOCAB_SIZE * TEMPL_PER_WORD)

enum {
    W_ZERO, W_ONE, W_TWO, W_THREE, W_FOUR, W_FIVE, W_SIX, W_SEVEN, W_EIGHT, W_NINE,
    W_PLUS, W_MINUS, W_MULTIPLY, W_EQUALS, W_CLEAR, W_CLOCK, W_TIMER, W_START, W_STOP
};

/* ---- endpointing (one frame = 256 samples = 16.384 ms) ---- */
#define EP_START_FRAMES   3     /* consecutive loud frames that start a word          */
#define EP_END_FRAMES     8     /* consecutive quiet frames that end it (131 ms)      */
#define EP_PREROLL        3     /* frames kept from before the first loud frame       */
#define EP_TRAIL          2     /* quiet frames kept after the last loud frame        */
#define EP_MIN_FRAMES     5
#define EP_MAX_FRAMES     50
#define EP_ON_MARGIN      27    /* loud  = energy > floor + this  (0.376 dB / count)  */
#define EP_OFF_MARGIN     16    /* quiet = energy < floor + this                      */

/* ---- DTW ---- */
#define DTW_BAND          8     /* Sakoe-Chiba half-width, frames                     */
#define DTW_DEFAULT_THR   2500  /* reject threshold (16 x L1 per path step) used      */
                                /* until calibration computes one                     */
#define DTW_REJECT_PCT    85    /* auto threshold = this % of the mean distance from  */
                                /* each template to its nearest different-word one    */
#define DTW_MARGIN_PCT    0     /* runner-up word must be this % worse (0 = disabled) */

/* ---- calibration ---- */
#define CAL_FULL_PASSES   TEMPL_PER_WORD  /* long press: wipe, then this many passes  */
#define BTN_LONG_MS       1500

/* ---- debug ---- */
#ifndef LOG_FEATURES
#define LOG_FEATURES      0     /* 1: print every captured word as hex on the UART    */
#endif

#endif
