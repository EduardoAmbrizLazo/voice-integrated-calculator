/* recog.c - see recog.h */
#include "recog.h"

int rec_classify(const dtw_seq_t *word, const dtw_seq_t *tmpl, const uint8_t *label, int nt, int32_t *dist)
{
    int32_t best = DTW_INF;
    int who = -1;
    for (int i = 0; i < nt; i++) {
        int32_t d = dtw_dist(word, &tmpl[i]);
        if (d < best) { best = d; who = label[i]; }
    }
    *dist = best;
    return who;
}

static uint32_t isqrt(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

int rec_loo_stats(const dtw_seq_t *tmpl, const uint8_t *label, int nt, int32_t *mean, int32_t *sd)
{
    int64_t sum = 0, sum2 = 0;
    int used = 0;
    for (int i = 0; i < nt; i++) {
        int32_t best = DTW_INF;
        for (int j = 0; j < nt; j++) {
            if (j == i || label[j] != label[i]) continue;
            int32_t d = dtw_dist(&tmpl[i], &tmpl[j]);
            if (d < best) best = d;
        }
        if (best >= DTW_INF) continue;                  /* no same-word partner to compare with */
        sum += best; sum2 += (int64_t)best * best; used++;
    }
    if (!used) { *mean = *sd = 0; return 0; }
    /* exact: var = (used * sum2 - sum^2) / used^2. (Using a rounded mean here would inflate it.) */
    *mean = (int32_t)((sum + used / 2) / used);
    int64_t var_num = (int64_t)used * sum2 - sum * sum;
    int64_t var = var_num > 0 ? var_num / ((int64_t)used * used) : 0;
    *sd = (int32_t)isqrt((uint64_t)var);
    return used;
}

int32_t rec_threshold(const dtw_seq_t *tmpl, const uint8_t *label, int nt, int k_x10)
{
    int32_t mean, sd;
    if (!rec_loo_stats(tmpl, label, nt, &mean, &sd)) return 0;       /* nothing usable: reject everything */
    return mean + (int32_t)(((int64_t)k_x10 * sd) / 10);
}
