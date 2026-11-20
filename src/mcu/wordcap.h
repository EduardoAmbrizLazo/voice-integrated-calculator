/* wordcap.h - finds a spoken word in the stream of feature frames and keeps its frames
 *
 * Detector (unchanged from the checkoff code):
 *   a frame is "loud" when its loudness is more than WC_ON_MARGIN above the noise floor,
 *   "quiet" when it is less than WC_OFF_MARGIN above it. 3 loud frames in a row start a
 *   word, 8 quiet frames in a row (131 ms) end it. The noise floor follows the room.
 *
 * Capture (new), same trimming as dtw_poc.py:
 *   the word's frames start WC_PRE frames before the first loud frame and stop at the
 *   last frame that was not part of the closing quiet stretch.
 *
 * No hardware in here, so it can be tested on a PC.
 */
#ifndef WORDCAP_H
#define WORDCAP_H

#include <stdint.h>
#include "frontend.h"

#ifndef WC_ON_MARGIN
#define WC_ON_MARGIN   27   /* a frame is "loud" this far above the noise floor (0.376 dB per count) */
#endif
#ifndef WC_OFF_MARGIN
#define WC_OFF_MARGIN  16   /* and "quiet" when it falls back below this                             */
#endif

#define WC_LOUD    3        /* loud frames in a row that start a word                     */
#define WC_QUIET   8        /* quiet frames in a row that end it                          */
#define WC_PRE     3        /* frames kept before the first loud one                      */
#define WC_MIN    12        /* fewer stored frames than this = a click or bump, not a word
                               (real words in the test set were 22..48 frames, a mouth click was 7) */
#define WC_MAXSPAN 50       /* loud-to-quiet span longer than this is rejected (0.8 s)    */
#define WC_MAXF    (WC_PRE + WC_MAXSPAN + 3)    /* most frames a kept word can have       */

typedef struct {
    uint8_t n;                          /* frames stored                                   */
    uint8_t f[WC_MAXF][FE_BANDS];       /* f[frame][band], straight from fe_frame_t.band   */
} wc_word_t;

/* result of wc_feed */
enum { WC_NONE = 0,     /* nothing to report yet                              */
       WC_WORD,         /* a word is ready in wc_word                         */
       WC_SHORT,        /* a click or bump, too short to be a word            */
       WC_LONG };       /* longer than WC_MAXSPAN: not a single word          */

extern wc_word_t wc_word;               /* the most recent word; valid after WC_WORD       */

void wc_init(void);
int  wc_feed(const fe_frame_t *fr);     /* call once per frame, in order                   */

int  wc_floor(void);                    /* noise floor, in loudness counts                 */
int  wc_take_peak(void);                /* loudest frame above the floor since last call   */
int  wc_word_ms(void);                  /* length of the last word in ms (loud part)       */

#endif
