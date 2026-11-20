/* recog.h - pick the closest enrolled word and decide whether to believe it */

#ifndef RECOG_H
#define RECOG_H

#include <stdint.h>
#include "dtw.h"

/* Compare a spoken word with nt templates. Returns the label of the closest one
 * (or -1 if none could be compared) and its distance in *dist (1/16 count). */
int rec_classify(const dtw_seq_t *word, const dtw_seq_t *tmpl, const uint8_t *label, int nt, int32_t *dist);

/* How far apart are two recordings of the SAME word? For each template, the distance to its
 * nearest same-word template; returns the mean and standard deviation of those distances
 * (1/16 count). Words with only one template are skipped. Returns the number of templates used. */
int rec_loo_stats(const dtw_seq_t *tmpl, const uint8_t *label, int nt, int32_t *mean, int32_t *sd);

/* Reject threshold: mean + k * sd of the above, with k given in tenths (30 = 3.0), as in dtw_poc.py. */
int32_t rec_threshold(const dtw_seq_t *tmpl, const uint8_t *label, int nt, int k_x10);


/* Default k for rec_threshold, in tenths. On the 56 captured test recordings (each left out in turn):
 *   k = 2.0 : 54 correct, 0 wrong, 2 rejected;  44 of 56 unknown words rejected
 *   k = 3.0 : 56 correct, 0 wrong, 0 rejected;  27 of 56 unknown words rejected  (dtw_poc.py's choice)
 * A wrong digit is worse than being asked to repeat it, and rejecting words outside the vocabulary is
 * a project goal, so 2.0 is the default. Raise it if real words get rejected too often. */
#define REC_K_X10  20

#endif
