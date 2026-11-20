/* WAV loading for the host tests: any PCM 16/24/32-bit or float WAV, mixed to
 * mono and resampled to the MCU's 15.625 kHz, scaled like the microphone's
 * 18-bit samples (16-bit full scale -> +-2^17). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav.h"
#include "recognizer.h"

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static double *read_mono(const char *path, int *n_out, int *sr_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    if (sz < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return NULL; }

    int fmt = 0, ch = 0, sr = 0, bits = 0;
    const uint8_t *data = NULL;
    uint32_t dlen = 0;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t len = rd32(b + p + 4);
        if (!memcmp(b + p, "fmt ", 4)) {
            fmt = rd16(b + p + 8); ch = rd16(b + p + 10); sr = (int)rd32(b + p + 12); bits = rd16(b + p + 22);
            if (fmt == 0xFFFE && len >= 40) fmt = rd16(b + p + 32);     /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(b + p, "data", 4)) {
            data = b + p + 8;
            dlen = len;
            if ((long)(p + 8 + dlen) > sz) dlen = (uint32_t)(sz - p - 8);
        }
        p += 8 + len + (len & 1);
    }
    if (!data || !ch || !(fmt == 1 || fmt == 3)) { free(b); return NULL; }
    int bps = bits / 8, n = (int)(dlen / (uint32_t)(bps * ch));
    double *x = malloc(sizeof(double) * (size_t)(n ? n : 1));
    for (int i = 0; i < n; i++) {
        double s = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *q = data + ((size_t)i * ch + c) * bps;
            double v;
            if (fmt == 3 && bits == 32) { float fl; memcpy(&fl, q, 4); v = fl; }
            else if (bits == 16) v = (int16_t)rd16(q) / 32768.0;
            else if (bits == 24) v = ((int32_t)(q[0] << 8 | q[1] << 16 | (uint32_t)q[2] << 24) >> 8) / 8388608.0;
            else if (bits == 32) v = (int32_t)rd32(q) / 2147483648.0;
            else if (bits == 8) v = (q[0] - 128) / 128.0;
            else { free(b); free(x); return NULL; }
            s += v;
        }
        x[i] = s / ch;
    }
    free(b);
    *n_out = n;
    *sr_out = sr;
    return x;
}

int32_t *wav_load_rate(const char *path, int rate, double cutoff, double full_scale, int *n_out)
{
    int n, sr;
    double *x = read_mono(path, &n, &sr);
    if (!x) return NULL;
    /* Windowed-sinc resampler with the given cutoff (Hz). */
    const double pi = 3.14159265358979323846;
    double step = (double)sr / rate, nc = 2.0 * cutoff / sr;
    if (nc > 1.0) nc = 1.0;
    int half = (int)ceil(16.0 / nc);
    int m = (int)((double)n / step);
    int32_t *y = malloc(sizeof(int32_t) * (size_t)(m ? m : 1));
    for (int i = 0; i < m; i++) {
        double t = i * step, s = 0;
        int c = (int)floor(t);
        for (int k = c - half + 1; k <= c + half; k++) {
            if (k < 0 || k >= n) continue;
            double u = t - k, a = pi * nc * u;
            double h = nc * (fabs(a) < 1e-9 ? 1.0 : sin(a) / a);
            double w = 0.5 + 0.5 * cos(pi * u / half);
            s += x[k] * h * w;
        }
        double v = s * full_scale;
        y[i] = (int32_t)lrint(v > full_scale - 1 ? full_scale - 1 : v < -full_scale ? -full_scale : v);
    }
    free(x);
    *n_out = m;
    return y;
}

/* REC_FS with a 7 kHz cutoff, 16-bit full scale -> +-2^17 like the mic's 18-bit samples. */
int32_t *wav_load(const char *path, int *n_out)
{
    return wav_load_rate(path, REC_FS, 7000.0, 131072.0, n_out);
}
