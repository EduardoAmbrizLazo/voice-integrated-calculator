/* wordcap.c - see wordcap.h */
#include "wordcap.h"

wc_word_t wc_word;

#define RING (WC_PRE + WC_LOUD)         /* frames needed back to the start of the pre-roll */

static uint8_t ring[RING][FE_BANDS];    /* the last RING frames, newest at ring[rpos - 1]  */
static unsigned rpos, rseen;

static int floor_q4 = -1;               /* noise floor x16                                 */
static int in_word, loud_run, quiet_run, frames, peak;
static int wlen;                        /* frames of this word so far, including pre-roll  */
static int len_ms;

void wc_init(void)
{
    rpos = rseen = 0;
    floor_q4 = -1;
    in_word = loud_run = quiet_run = frames = peak = wlen = len_ms = 0;
    wc_word.n = 0;
}

int  wc_floor(void)    { return floor_q4 < 0 ? 0 : floor_q4 >> 4; }
int  wc_word_ms(void)  { return len_ms; }
int  wc_take_peak(void){ int p = peak; peak = 0; return p; }

static void store(const uint8_t *b)
{
    if (wlen < WC_MAXF) {
        for (int k = 0; k < FE_BANDS; k++) wc_word.f[wlen][k] = b[k];
    }
    wlen++;
}

int wc_feed(const fe_frame_t *fr)
{
    int e = fr->loud;

    for (int k = 0; k < FE_BANDS; k++) ring[rpos][k] = fr->band[k];
    rpos = (rpos + 1) % RING;
    if (rseen < RING) rseen++;

    if (floor_q4 < 0) floor_q4 = e << 4;
    int fl = floor_q4 >> 4;
    int loud = e > fl + WC_ON_MARGIN, quiet = e < fl + WC_OFF_MARGIN;
    if (e - fl > peak) peak = e - fl;

    if (!in_word) {
        if (loud) {
            if (++loud_run >= WC_LOUD) {
                in_word = 1; frames = loud_run; quiet_run = 0;
                /* start the word with everything in the ring: the pre-roll and the loud frames */
                unsigned i = (rpos + RING - rseen) % RING;
                wlen = 0;
                for (unsigned n = 0; n < rseen; n++) { store(ring[i]); i = (i + 1) % RING; }
            }
        } else {
            loud_run = 0;                       /* follow the room: down fast, up slowly */
            floor_q4 += ((e << 4) - floor_q4) / (((e << 4) < floor_q4) ? 4 : 32);
        }
        return WC_NONE;
    }

    frames++;
    store(fr->band);
    quiet_run = quiet ? quiet_run + 1 : 0;
    if (frames > 60) floor_q4 += 8;             /* stuck "loud": the room got noisier */
    if (quiet_run < WC_QUIET) return WC_NONE;

    in_word = 0; loud_run = 0;
    int span = frames - WC_QUIET;               /* loud part, without the closing quiet stretch */
    len_ms = span * 16;
    if (span > WC_MAXSPAN) return WC_LONG;
    int n = wlen - WC_QUIET;                    /* drop the closing quiet frames from the stored word */
    if (n > WC_MAXF) n = WC_MAXF;
    if (n < WC_MIN) return WC_SHORT;
    wc_word.n = (uint8_t)n;
    return WC_WORD;
}
