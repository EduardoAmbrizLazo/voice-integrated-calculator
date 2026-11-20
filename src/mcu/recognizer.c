/* Speech recognizer, see recognizer.h.  Each section names the dtw_poc.py
 * function it reproduces. */
#include <math.h>
#include <string.h>
#include "recognizer.h"

const char *const rec_word_name[REC_NWORDS] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "plus", "minus", "multiply", "equals", "clear", "clock", "timer", "start", "stop"
};

/* Tuned with tests/host/rec_eval on src/proofofconcept/data (91% vs 88% for
 * midpointmain.c's 27/16/8). */
rec_cfg_t   rec_cfg = { 27, 21, 3, 10, 3, 0 };
rec_stats_t rec_stats;

#define DTW_BAND    8                   /* Sakoe-Chiba half-width, frames            */
#define LOG_OFFSET  80                  /* feature = 8*log2(sum of squares) - this   */
#define BUF_FRAMES  64                  /* word buffer: max + quiet tail + spare     */
#define HIST        16                  /* frames remembered before a word starts    */

/* ------------------------------------------------------------- filter bank */
/* make_filterbank(): 12 log-spaced constant-Q peak biquads (scipy iirpeak).
 * Direct form I with Q30 coefficients and a 64-bit accumulator.  b1 = 0 and
 * b2 = -b0 for a peak filter, so y = b0*(x - x2) - a1*y1 - a2*y2. */
static int32_t  b0_q30[REC_NB], na1_q30[REC_NB], na2_q30[REC_NB];
static int32_t  fx1, fx2, fy1[REC_NB], fy2[REC_NB];   /* filter state              */
static int32_t  dc_x1, dc_y1;                         /* DC blocker state          */
static uint64_t band_acc[REC_NB], tot_acc;
static int      nsamp;

void rec_init(void)
{
    const double f_lo = 200.0, f_hi = 6000.0, pi = 3.14159265358979323846;
    double r = pow(f_hi / f_lo, 1.0 / REC_NB);
    double q = 1.0 / (sqrt(r) - 1.0 / sqrt(r));
    for (int k = 0; k < REC_NB; k++) {
        double fc = f_lo * pow(r, k + 0.5);
        double w0 = 2.0 * pi * fc / REC_FS;
        double g = 1.0 / (1.0 + tan(w0 / q / 2.0));
        b0_q30[k]  = (int32_t)lround((1.0 - g) * 1073741824.0);
        na1_q30[k] = (int32_t)lround(2.0 * g * cos(w0) * 1073741824.0);
        na2_q30[k] = (int32_t)lround(-(2.0 * g - 1.0) * 1073741824.0);
    }
    rec_reset();
}

/* 8 * log2(v), rounded: 1 count = 0.376 dB.  Mantissa from 8 bits after the
 * leading one, with a parabola correcting the straight-line log2(1+x). */
static int log2_q3(uint64_t v)
{
    if (!v) return 0;
    int p = 63 - __builtin_clzll(v);
    uint32_t m = (p >= 8) ? (uint32_t)(v >> (p - 8)) & 0xFF : (uint32_t)(v << (8 - p)) & 0xFF;
    uint32_t q8 = (uint32_t)p * 256 + m + ((m * (256 - m) * 89) >> 16);
    return (int)((q8 + 16) >> 5);
}

static uint8_t to_feature(uint64_t sum)
{
    int v = log2_q3(sum) - LOG_OFFSET;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* ------------------------------------------------------------- endpointer */
/* find_segments() + extract_word(), rewritten to run one frame at a time:
 * the noise floor follows the room (down fast, up slowly) instead of being a
 * percentile of a whole recording. */
static uint8_t  hist[HIST][REC_NB];     /* ring of recent frames                  */
static int      hist_n;                 /* frames written into the ring           */
static uint8_t  buf[BUF_FRAMES][REC_NB];
static int      cnt, last_active, loud_run, quiet_run, in_word;
static int      floor_q4 = -1;
static rec_tmpl_t done;                 /* finished word, waiting for rec_poll    */
static volatile int done_ready;

void rec_reset(void)
{
    fx1 = fx2 = 0;
    memset(fy1, 0, sizeof fy1); memset(fy2, 0, sizeof fy2);
    dc_x1 = dc_y1 = 0;
    memset(band_acc, 0, sizeof band_acc);
    tot_acc = 0; nsamp = 0;
    hist_n = 0; cnt = 0; loud_run = 0; quiet_run = 0; in_word = 0;
    floor_q4 = -1;
    done_ready = 0;
    rec_stats.in_word = 0;
}

static void finish_word(void)
{
    int len = last_active + 1;
    if (len < REC_MIN_FRAMES) return;
    if (len > REC_MAX_FRAMES) len = REC_MAX_FRAMES;
    if (done_ready) { rec_stats.dropped++; return; }
    memset(&done, 0, sizeof done);
    done.len = (uint8_t)len;
    for (int k = 0; k < REC_NB; k++) {
        int sum = 0;
        for (int i = 0; i < len; i++) sum += buf[i][k];
        int mean = (sum + len / 2) / len;
        for (int i = 0; i < len; i++) {
            int v = buf[i][k] - mean;
            done.f[i][k] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
        }
    }
    done_ready = 1;
}

static void on_frame(const uint8_t *q, int e)
{
    const rec_cfg_t *c = &rec_cfg;
    if (floor_q4 < 0) floor_q4 = e << 4;
    int fl = floor_q4 >> 4;
    rec_stats.level = e;
    rec_stats.floor = fl;
    rec_stats.frames++;

    if (!in_word) {
        int loud = e > fl + c->on_margin && e > c->abs_min;
        memcpy(hist[hist_n % HIST], q, REC_NB);
        hist_n++;
        if (loud) {
            if (++loud_run >= c->loud_frames) {
                int n0 = c->loud_frames + c->preroll;
                if (n0 > hist_n) n0 = hist_n;
                if (n0 > HIST) n0 = HIST;
                for (int i = 0; i < n0; i++)
                    memcpy(buf[i], hist[(hist_n - n0 + i) % HIST], REC_NB);
                cnt = n0;
                last_active = n0 - 1;
                quiet_run = 0;
                in_word = 1;
            }
        } else {
            loud_run = 0;
            int d = (e << 4) - floor_q4;
            floor_q4 += d / (d < 0 ? 4 : 32);
        }
        rec_stats.in_word = in_word;
        return;
    }

    if (cnt < BUF_FRAMES) memcpy(buf[cnt], q, REC_NB);
    cnt++;
    if (e >= fl + c->off_margin) { last_active = cnt - 1; quiet_run = 0; }
    else quiet_run++;
    if (cnt > 60) floor_q4 += 8;                    /* stuck "loud": the room got noisier */
    if (quiet_run >= c->quiet_frames) {
        in_word = 0; loud_run = 0; hist_n = 0;
        if (last_active >= BUF_FRAMES) last_active = BUF_FRAMES - 1;
        finish_word();
    }
    rec_stats.in_word = in_word;
}

/* band_features(), one sample at a time. */
void rec_push(const int32_t *x, int n)
{
    for (int s = 0; s < n; s++) {
        /* DC blocker: y = x - x[-1] + 0.995 y[-1] */
        int32_t xi = x[s];
        int32_t y = xi - dc_x1 + (int32_t)(((int64_t)dc_y1 * 32604 + 16384) >> 15);
        dc_x1 = xi; dc_y1 = y;
        tot_acc += (uint64_t)((int64_t)y * y);

        int32_t d = y - fx2;
        for (int k = 0; k < REC_NB; k++) {
            int64_t acc = (int64_t)b0_q30[k] * d + (int64_t)na1_q30[k] * fy1[k] +
                          (int64_t)na2_q30[k] * fy2[k];
            int32_t o = (int32_t)((acc + (1 << 29)) >> 30);
            fy2[k] = fy1[k]; fy1[k] = o;
            band_acc[k] += (uint64_t)((int64_t)o * o);
        }
        fx2 = fx1; fx1 = y;

        if (++nsamp == REC_FRAME) {
            uint8_t q[REC_NB];
            for (int k = 0; k < REC_NB; k++) { q[k] = to_feature(band_acc[k]); band_acc[k] = 0; }
            int e = to_feature(tot_acc);
            tot_acc = 0; nsamp = 0;
            on_frame(q, e);
        }
    }
}

int rec_poll(rec_tmpl_t *out)
{
    if (!done_ready) return 0;
    *out = done;
    done_ready = 0;
    return 1;
}

/* --------------------------------------------------------------------- DTW */
/* _dtw_cost() + dtw_distance(): banded symmetric DTW, L1 frame distance,
 * result = total cost / (n + m) / bands, in Q8 feature counts. */
static uint32_t frame_dist(const int8_t *a, const int8_t *b)
{
    uint32_t s = 0;
    for (int k = 0; k < REC_NB; k++) { int v = a[k] - b[k]; s += (uint32_t)(v < 0 ? -v : v); }
    return s;
}

uint32_t rec_dist(const rec_tmpl_t *a, const rec_tmpl_t *b)
{
    int n = a->len, m = b->len;
    if (n == 0 || m == 0 || n > REC_MAX_FRAMES || m > REC_MAX_FRAMES) return REC_INF;
    if (n > 2 * m || m > 2 * n) return REC_INF;
    uint32_t r0[REC_MAX_FRAMES], r1[REC_MAX_FRAMES], *prev = r0, *cur = r1;
    int den = n > 1 ? n - 1 : 1;

    for (int i = 0; i < n; i++) {
        int cn = n > 1 ? i * (m - 1) : 0;           /* diagonal = cn / den   */
        int lo = cn - DTW_BAND * den, hi = cn + DTW_BAND * den;
        int jlo = lo <= 0 ? 0 : (lo + den - 1) / den;
        int jhi = hi / den;
        if (jhi > m - 1) jhi = m - 1;
        for (int j = 0; j < m; j++) cur[j] = REC_INF;
        for (int j = jlo; j <= jhi; j++) {
            uint32_t d = frame_dist(a->f[i], b->f[j]), best = REC_INF, v;
            if (i == 0 && j == 0) { cur[0] = 2 * d; continue; }
            if (i > 0) {
                if (prev[j] != REC_INF && (v = prev[j] + d) < best) best = v;
                if (j > 0 && prev[j - 1] != REC_INF && (v = prev[j - 1] + 2 * d) < best) best = v;
            }
            if (j > 0 && cur[j - 1] != REC_INF && (v = cur[j - 1] + d) < best) best = v;
            cur[j] = best;
        }
        uint32_t *t = prev; prev = cur; cur = t;
    }
    uint32_t c = prev[m - 1];
    if (c == REC_INF) return REC_INF;
    return (uint32_t)(((uint64_t)c * 256) / (uint32_t)((n + m) * REC_NB));
}

/* --------------------------------------------------------------- classify */
static int slot_ok(const rec_tmpl_t *t) { return t->len >= 1 && t->len <= REC_MAX_FRAMES; }

int rec_store_valid(const rec_store_t *s)
{
    return s && s->magic == REC_STORE_MAGIC && s->tpw == REC_TPW;
}

void rec_classify(const rec_tmpl_t *w, const rec_store_t *s, rec_result_t *r)
{
    r->word = r->best = r->second = -1;
    r->dist = r->dist2 = REC_INF;
    if (!s) return;
    for (int wi = 0; wi < REC_NWORDS; wi++) {
        uint32_t dw = REC_INF;
        for (int k = 0; k < REC_TPW; k++) {
            const rec_tmpl_t *t = &s->t[wi][k];
            if (!slot_ok(t)) continue;
            uint32_t d = rec_dist(w, t);
            if (d < dw) dw = d;
        }
        if (dw < r->dist) { r->dist2 = r->dist; r->second = r->best; r->dist = dw; r->best = wi; }
        else if (dw < r->dist2) { r->dist2 = dw; r->second = wi; }
    }
    if (r->best >= 0 && r->dist <= s->thr_q8) r->word = r->best;
}

/* loo_threshold(): for each template, the distance to its nearest same-word
 * template; threshold = mean + k * std.  k_q4 = 48 means k = 3.0. */
uint32_t rec_threshold(const rec_store_t *s, int k_q4)
{
    uint64_t sum = 0, sum2 = 0;
    uint32_t n = 0;
    for (int wi = 0; wi < REC_NWORDS; wi++)
        for (int a = 0; a < REC_TPW; a++) {
            if (!slot_ok(&s->t[wi][a])) continue;
            uint32_t best = REC_INF;
            for (int b = 0; b < REC_TPW; b++) {
                if (b == a || !slot_ok(&s->t[wi][b])) continue;
                uint32_t d = rec_dist(&s->t[wi][a], &s->t[wi][b]);
                if (d < best) best = d;
            }
            if (best == REC_INF) continue;
            sum += best; sum2 += (uint64_t)best * best; n++;
        }
    if (!n) return REC_INF;                     /* nothing to compare: accept everything */
    uint64_t mean = sum / n;
    uint64_t var = sum2 / n - mean * mean;
    uint64_t sd = 0;                            /* integer square root */
    for (uint64_t bit = 1ull << 30; bit; bit >>= 1)
        if ((sd + bit) * (sd + bit) <= var) sd += bit;
    return (uint32_t)(mean + ((sd * (uint64_t)k_q4) >> 4));
}
