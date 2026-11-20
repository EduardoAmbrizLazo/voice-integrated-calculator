#ifndef WAV_H
#define WAV_H
#include <stdint.h>

/* Load a WAV as mono samples at REC_FS, scaled like the mic (+-2^17).
 * Returns malloc'd samples, or NULL if the file can't be read. */
int32_t *wav_load(const char *path, int *n_out);

/* Same, at any sample rate: cutoff in Hz, full_scale = value of a full-scale sample. */
int32_t *wav_load_rate(const char *path, int rate, double cutoff, double full_scale, int *n_out);

#endif
