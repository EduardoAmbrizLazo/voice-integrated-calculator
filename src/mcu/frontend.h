/* frontend.h - microphone samples -> one 12-band feature frame every 16 ms
 *
 * Same chain as dtw_poc.py:
 *   62.5 kHz mic samples -> 64-tap FIR, /4 -> 15.625 kHz -> DC blocker
 *   -> 12 band-pass biquads (200..6000 Hz, log spaced) -> energy per 256 samples
 *   -> log scale, 0..255, 2.55 counts per dB
 *
 * No hardware in here, so it can be tested on a PC (see run_host_test.py).
 */
#ifndef FRONTEND_H
#define FRONTEND_H

#include <stdint.h>

#define FE_BANDS   12
#define FE_FRAME   256      /* output samples per frame: 256 / 15.625 kHz = 16.4 ms */

typedef struct {
    uint8_t band[FE_BANDS]; /* log energy of each band; 2.55 counts per dB (3.60 dB = 9 counts)   */
    uint8_t loud;           /* whole-signal loudness, 8 counts per doubling of energy (0.376 dB),
                               used only to find where a word starts and ends                     */
} fe_frame_t;

void fe_init(void);

/* 1 = skip the anti-alias FIR (just keep every 4th sample), for the "filter bypass" button */
void fe_set_bypass(int on);

/* Feed one mic sample (signed 16 bit, i.e. the 18-bit I2S sample >> 2), at 62.5 kHz.
 * Returns 1 when a frame has just finished and fills *out; otherwise returns 0. */
int fe_push(int16_t x, fe_frame_t *out);

#endif
