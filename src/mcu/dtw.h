/* dtw.h - dynamic time warping between two words, as in dtw_poc.py
 *
 * A word is a block of frames from ceps_word(): 11 signed bytes per frame, the word's own
 * average already removed (so loudness and microphone distance mostly drop out).
 *
 * Distance = mean absolute difference per value per step along the best warping path
 * (Sakoe-Chiba band of +/-8 frames, diagonal steps count double, divided by path length).
 * Integer maths only. No hardware in here, so it can be tested on a PC.
 */
#ifndef DTW_H
#define DTW_H

#include <stdint.h>
#include "ceps.h"

#define DTW_BAND    8           /* warping window half-width, frames                       */
#define DTW_MAXLEN  64          /* longest word handled                                    */
#define DTW_INF     0x3FFFFFFF  /* "cannot be compared" (lengths too different)            */

/* distances are in 1/16 of a count (a count is 0.39 dB); one dB is 2.55 counts = 40.8 of these units */
#define DTW_UNITS_PER_DB  40.8f

typedef struct {
    const int8_t (*f)[CEP_DIM];     /* n frames, from ceps_word() */
    int n;
} dtw_seq_t;

/* mean |difference| per value per step, in 1/16 count; DTW_INF if the words differ in length by more than 2x */
int32_t dtw_dist(const dtw_seq_t *a, const dtw_seq_t *b);

#endif
