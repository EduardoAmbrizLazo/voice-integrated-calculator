/* dtw.c - see dtw.h */
#include "dtw.h"

int32_t dtw_dist(const dtw_seq_t *a, const dtw_seq_t *b)
{
    const int n = a->n, m = b->n;
    if (n < 1 || m < 1 || n > DTW_MAXLEN || m > DTW_MAXLEN) return DTW_INF;
    if (n > 2 * m || m > 2 * n) return DTW_INF;            /* lengths too different: no match */

    /* static, not on the stack: the STM32's stack is small. (So this is not re-entrant; only the main loop calls it.) */
    static int32_t row0[DTW_MAXLEN], row1[DTW_MAXLEN];
    int32_t *prev = row0, *cur = row1;
    const int den = n > 1 ? n - 1 : 1;

    for (int i = 0; i < n; i++) {
        /* columns j allowed on this row: within DTW_BAND of the length-scaled diagonal */
        int jlo = 0, jhi = DTW_BAND;
        if (n > 1) {
            int num = i * (m - 1);
            int lo = num - DTW_BAND * den;
            jlo = lo <= 0 ? 0 : (lo + den - 1) / den;      /* ceil */
            jhi = (num + DTW_BAND * den) / den;            /* floor */
        }
        if (jhi > m - 1) jhi = m - 1;

        for (int j = 0; j < m; j++) cur[j] = DTW_INF;
        for (int j = jlo; j <= jhi; j++) {
            int32_t d = 0;                                  /* frame distance: sum of |differences| */
            for (int k = 0; k < CEP_DIM; k++) {
                int32_t x = (int32_t)a->f[i][k] - (int32_t)b->f[j][k];
                d += x < 0 ? -x : x;
            }
            if (i == 0 && j == 0) { cur[0] = 2 * d; continue; }
            int32_t best = DTW_INF, v;
            if (i > 0) {
                if (prev[j] < DTW_INF)                { v = prev[j] + d;         if (v < best) best = v; }
                if (j > 0 && prev[j - 1] < DTW_INF)   { v = prev[j - 1] + 2 * d; if (v < best) best = v; }
            }
            if (j > 0 && cur[j - 1] < DTW_INF)        { v = cur[j - 1] + d;      if (v < best) best = v; }
            cur[j] = best;
        }
        int32_t *t = prev; prev = cur; cur = t;
    }
    if (prev[m - 1] >= DTW_INF) return DTW_INF;
    /* path weight is n+m, and there are CEP_DIM values per frame; answer in 1/16 count */
    const int32_t norm = (n + m) * CEP_DIM;
    return (prev[m - 1] * 16 + norm / 2) / norm;
}
