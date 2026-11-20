/* ceps.h - turns a captured word into the form the matcher compares
 *
 * Each 12-band frame goes through a DCT (the MFCC-style step): the 12 band energies become
 * 11 "cepstral" values that describe the SHAPE of the spectrum, without the overall level.
 * Then the word's own average of each value is taken off, and the result is stored as signed
 * bytes, one count per step (a count is 0.39 dB). In the recordings tested, the largest value
 * was 118, so nothing clips. 11 bytes per frame: 57 words of ~30 frames is about 19 KB.
 *
 * On the 56 test words this cut wrong answers from 5 to 0 (vs comparing the 12 bands directly)
 * and made unknown words much easier to reject. No hardware in here, so it can be tested on a PC.
 */
#ifndef CEPS_H
#define CEPS_H

#include <stdint.h>
#include "frontend.h"

#define CEP_DIM  (FE_BANDS - 1)

/* bands: n frames from wordcap (wc_word.f); out: n frames of CEP_DIM signed bytes */
void ceps_word(const uint8_t (*bands)[FE_BANDS], int n, int8_t (*out)[CEP_DIM]);

#endif
