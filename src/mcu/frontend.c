/* frontend.c - see frontend.h. Integer maths only (no FPU setup needed). */
#include "frontend.h"
#include "fe_tables.h"

/* ----------------------------------------------------------------- state */
static int16_t  hist[2 * FIR_N];        /* last FIR_N inputs, stored twice so a window never wraps */
static unsigned hp;                     /* index of the oldest sample in the window                */
static unsigned phase;                  /* counts input samples 0..3 for the /4                    */
static int      bypass;

static int32_t  dc_x;                   /* previous DC-blocker input                               */
static int32_t  dc_y;                   /* DC-blocker output, Q8 (8 fractional bits)               */
static int32_t  bx1, bx2;               /* last two band-bank inputs (shared by all 12 bands), Q8  */
static int32_t  by1[FE_BANDS], by2[FE_BANDS];           /* band outputs, Q8                        */
static uint64_t en[FE_BANDS];           /* sum of squares in this frame                            */
static uint64_t en_total;
static unsigned nout;                   /* output samples so far in this frame                     */

void fe_init(void)
{
    for (unsigned i = 0; i < 2 * FIR_N; i++) hist[i] = 0;
    hp = 0; phase = 0; nout = 0;
    dc_x = dc_y = bx1 = bx2 = 0;
    for (int k = 0; k < FE_BANDS; k++) { by1[k] = by2[k] = 0; en[k] = 0; }
    en_total = 0;
}

void fe_set_bypass(int on) { bypass = on; }

/* --------------------------------------------------------------- log maths */
/* 8 x log2(v): 8 counts per doubling of energy (same scale the word detector always used) */
static int log8(uint64_t v)
{
    if (!v) return 0;
    int p = 63 - __builtin_clzll(v);
    uint32_t frac = (p >= 3) ? (uint32_t)((v >> (p - 3)) & 7) : (uint32_t)((v << (3 - p)) & 7);
    return 8 * p + (int)frac;
}

/* 256 x log2(v) */
static uint32_t log2_q8(uint64_t v)
{
    int p = 63 - __builtin_clzll(v);
    uint32_t m = (p >= 8) ? (uint32_t)((v >> (p - 8)) & 0xFF) : (uint32_t)((v << (8 - p)) & 0xFF);
    return (uint32_t)p * 256u + fe_log2[m];
}

/* band energy -> 0..255 feature, same scale as the Python model: (dB + 100) * 2.55 */
static uint8_t to_count(uint64_t s)
{
    if (s < 2) return 0;
    int32_t c8 = (int32_t)(((uint64_t)log2_q8(s) * (uint32_t)FE_QK16) >> 16) - FE_QOFF8;
    int32_t c  = (c8 + 128) >> 8;
    return (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c);
}

/* ------------------------------------------------------------------ chain */
static void finish_frame(fe_frame_t *out)
{
    for (int k = 0; k < FE_BANDS; k++) { out->band[k] = to_count(en[k]); en[k] = 0; }
    int l = log8(en_total >> 16);               /* >>16 undoes the two Q8 factors */
    out->loud = (uint8_t)(l > 255 ? 255 : l);
    en_total = 0;
    nout = 0;
}

int fe_push(int16_t xin, fe_frame_t *out)
{
    hist[hp] = xin;
    hist[hp + FIR_N] = xin;
    hp = (hp + 1) & (FIR_N - 1);
    if (++phase < 4) return 0;                  /* keep one sample in four */
    phase = 0;

    /* 1. anti-alias FIR + decimate (symmetric taps: add mirrored samples first) */
    int32_t y;
    if (bypass) {
        y = xin;
    } else {
        const int16_t *p = &hist[hp];           /* oldest .. newest */
        int32_t acc = 0;
        for (int k = 0; k < FIR_N / 2; k++)
            acc += (int32_t)fe_fir[k] * ((int32_t)p[k] + (int32_t)p[FIR_N - 1 - k]);
        y = (acc + (1 << 14)) >> 15;
        if (y >  32767) y =  32767;
        if (y < -32768) y = -32768;
    }

    /* 2. DC blocker y[n] = x[n] - x[n-1] + 0.995 y[n-1], kept with 8 extra bits */
    dc_y = (int32_t)(((int64_t)dc_y * FE_DC_POLE_Q15) >> 15) + ((y - dc_x) << 8);
    dc_x = y;
    int32_t x0 = dc_y;

    /* 3. the 12 band-pass biquads: y = b0 (x - x[n-2]) - a1 y[n-1] - a2 y[n-2] */
    int32_t d = x0 - bx2;
    for (int k = 0; k < FE_BANDS; k++) {
        int64_t acc = (int64_t)fe_bank[k][0] * d
                    - (int64_t)fe_bank[k][1] * by1[k]
                    - (int64_t)fe_bank[k][2] * by2[k];
        int32_t yy = (int32_t)((acc + (1 << 28)) >> 29);
        by2[k] = by1[k];
        by1[k] = yy;
        en[k] += (uint64_t)((int64_t)yy * yy);
    }
    bx2 = bx1;
    bx1 = x0;
    en_total += (uint64_t)((int64_t)x0 * x0);

    if (++nout < FE_FRAME) return 0;
    finish_frame(out);
    return 1;
}
