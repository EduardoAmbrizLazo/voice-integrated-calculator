/* Speech recognizer: C port of src/proofofconcept/dtw_poc.py.
 *
 *   samples (15.625 kHz) -> DC block -> 12 band-pass biquads (200..6000 Hz)
 *     -> 256-sample frames (16.4 ms) -> log2 energy per band, 8 bits
 *     -> online endpointing (adaptive noise floor)
 *     -> subtract the word's mean per band
 *     -> banded DTW against stored templates -> best word, or reject
 *
 * No hardware access and integer math only (except rec_init, which computes
 * the filter coefficients once).  The same file runs on the STM32 and in the
 * host tests under tests/host.
 *
 * Threading on the MCU: rec_push() runs in the audio interrupt.  Everything
 * else runs in the main loop.
 */
#ifndef RECOGNIZER_H
#define RECOGNIZER_H
#include <stdint.h>

#define REC_FS          15625   /* sample rate fed to rec_push (62.5 kHz / 4)        */
#define REC_NB          12      /* frequency bands                                   */
#define REC_FRAME       256     /* samples per frame (16.4 ms)                       */
#define REC_MAX_FRAMES  50      /* longest word kept (0.82 s); longer is truncated   */
#define REC_MIN_FRAMES  5       /* shorter sounds are ignored (clicks)               */
#define REC_NWORDS      19
#ifndef REC_TPW
#define REC_TPW         3       /* templates stored per word: 19 x 3 = 57          */
#endif

/* Word indices.  Order matches WORDS in dtw_poc.py. */
enum {
    W_ZERO, W_ONE, W_TWO, W_THREE, W_FOUR, W_FIVE, W_SIX, W_SEVEN, W_EIGHT, W_NINE,
    W_PLUS, W_MINUS, W_MULTIPLY, W_EQUALS, W_CLEAR, W_CLOCK, W_TIMER, W_START, W_STOP
};
extern const char *const rec_word_name[REC_NWORDS];   /* "zero" ... "stop" */

/* One spoken word: mean-subtracted features, 1 count = 0.376 dB. */
typedef struct {
    uint8_t len;                                /* frames; 0 or 0xFF = empty slot */
    uint8_t word;                               /* which word (templates only)    */
    int8_t  f[REC_MAX_FRAMES][REC_NB];
    uint8_t pad[6];                             /* size 608: a multiple of 8 for flash */
} rec_tmpl_t;

/* Everything enrollment saves.  Lives in flash on the MCU (read in place). */
#define REC_STORE_MAGIC 0x56494331u            /* "VIC1" */
typedef struct {
    uint32_t   magic;                           /* REC_STORE_MAGIC once calibrated */
    uint32_t   thr_q8;                          /* reject threshold, see rec_dist  */
    uint32_t   tpw;                             /* REC_TPW at the time of writing  */
    uint32_t   reserved;
    rec_tmpl_t t[REC_NWORDS][REC_TPW];
} rec_store_t;

/* Endpointer tuning, in counts of 0.376 dB.  Defaults in recognizer.c. */
typedef struct {
    int on_margin;      /* frame is "loud" this far above the noise floor (27 = 10 dB) */
    int off_margin;     /* frame still counts as speech above this (21 = 8 dB)        */
    int loud_frames;    /* consecutive loud frames that start a word (3)              */
    int quiet_frames;   /* consecutive quiet frames that end it (10 = 164 ms)         */
    int preroll;        /* frames kept from before the start (3)                      */
    int abs_min;        /* never call a frame loud below this total level (0 = off)   */
} rec_cfg_t;
extern rec_cfg_t rec_cfg;

typedef struct {
    int     word;       /* best word, or -1 if rejected / no templates */
    int     best;       /* index of best word even when rejected       */
    int     second;     /* runner-up word                              */
    uint32_t dist;      /* best distance (Q8 counts per band per step) */
    uint32_t dist2;     /* runner-up distance                          */
} rec_result_t;

#define REC_INF 0xFFFFFFFFu

void     rec_init(void);                        /* also resets the detector           */
void     rec_reset(void);                       /* forget any partial word / pending  */
void     rec_push(const int32_t *x, int n);     /* samples, ~18-bit full scale (+-2^17) */
int      rec_poll(rec_tmpl_t *out);             /* 1 = a word ended; features in *out */

uint32_t rec_dist(const rec_tmpl_t *a, const rec_tmpl_t *b);   /* REC_INF if no match */
void     rec_classify(const rec_tmpl_t *w, const rec_store_t *s, rec_result_t *r);
uint32_t rec_threshold(const rec_store_t *s, int k_q4);        /* mean + k*std, k in 1/16 */
int      rec_store_valid(const rec_store_t *s);

/* For the level meter and debugging (written by rec_push). */
typedef struct {
    volatile int      level;    /* last frame's total level   */
    volatile int      floor;    /* noise floor                */
    volatile int      in_word;
    volatile uint32_t frames;   /* frames processed            */
    volatile uint32_t dropped;  /* words lost because the last one was not collected */
} rec_stats_t;
extern rec_stats_t rec_stats;

#endif
