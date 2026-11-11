/* Arthur Cary
 *
 * Distance: L1 over the 12 band energies.  Path: symmetric steps
 * (i-1,j) (i,j-1) weight 1, (i-1,j-1) weight 2, restricted to DTW_BAND frames
 * either side of the length-scaled diagonal.  Total / (n + m) = cost per step.
 * Templates live in RAM (g_store) while running and are mirrored to flash. */
#include <string.h>
#include "dtw.h"
#include "hal.h"

#define STORE_MAGIC 0x56434C32u     /* "VCL2" */

store_t g_store;

static uint32_t store_sum(void)
{
    const uint8_t *p = (const uint8_t *)&g_store + 8;
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < sizeof g_store - 8; i++) { a += p[i]; b += a; }
    return (b << 16) ^ a;
}

void dtw_store_wipe(void)
{
    memset(&g_store, 0, sizeof g_store);
    g_store.magic = STORE_MAGIC;
    g_store.reject_thr = DTW_DEFAULT_THR;
}

bool dtw_store_load(void)
{
    hal_store_read(&g_store, sizeof g_store);
    if (g_store.magic == STORE_MAGIC && g_store.checksum == store_sum()) return true;
    dtw_store_wipe();
    return false;
}

void dtw_normalize(const word_t *w, int8_t out[][NBANDS])
{
    for (int b = 0; b < NBANDS; b++) {
        int32_t sum = 0;
        for (int i = 0; i < w->n; i++) sum += w->f[i][b];
        int mean = (sum + w->n / 2) / w->n;
        for (int i = 0; i < w->n; i++) {
            int v = (int)w->f[i][b] - mean;
            out[i][b] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
        }
    }
}

uint32_t dtw_distance(const int8_t a[][NBANDS], int n, const int8_t b[][NBANDS], int m,
                      uint32_t abandon)
{
    static uint32_t row0[EP_MAX_FRAMES], row1[EP_MAX_FRAMES];
    uint32_t *prev = row0, *cur = row1;

    if (n < 1 || m < 1 || n > 2 * m || m > 2 * n) return DTW_INF;
    /* abandon is a x16 normalised distance; convert to a raw path sum */
    uint32_t limit = (abandon >= DTW_INF / 64) ? DTW_INF
                                               : (abandon * (uint32_t)(n + m)) / 16 + 1;

    for (int i = 0; i < n; i++) {
        int c  = (n > 1) ? (i * (m - 1) + (n - 1) / 2) / (n - 1) : 0;
        int lo = c - DTW_BAND < 0 ? 0 : c - DTW_BAND;
        int hi = c + DTW_BAND > m - 1 ? m - 1 : c + DTW_BAND;
        uint32_t rowmin = DTW_INF;

        for (int j = 0; j < m; j++) cur[j] = DTW_INF;
        for (int j = lo; j <= hi; j++) {
            uint32_t d = 0;
            for (int k = 0; k < NBANDS; k++) {
                int x = a[i][k] - b[j][k];
                d += (uint32_t)(x < 0 ? -x : x);
            }
            uint32_t best;
            if (i == 0 && j == 0) {
                best = 2 * d;
            } else {
                best = DTW_INF;
                if (j > 0 && cur[j - 1] + d < best)             best = cur[j - 1] + d;
                if (i > 0 && prev[j] + d < best)                best = prev[j] + d;
                if (i > 0 && j > 0 && prev[j - 1] + 2 * d < best) best = prev[j - 1] + 2 * d;
                if (best > DTW_INF) best = DTW_INF;
            }
            cur[j] = best;
            if (best < rowmin) rowmin = best;
        }
        if (rowmin >= limit) return DTW_INF;          /* cannot beat the best so far */
        uint32_t *t = prev; prev = cur; cur = t;
    }
    uint32_t total = prev[m - 1];
    if (total >= DTW_INF) return DTW_INF;
    return (total * 16u) / (uint32_t)(n + m);
}

void dtw_enroll(int word, const word_t *w)
{
    templ_t *slot = &g_store.t[word * TEMPL_PER_WORD];
    templ_t *dst = slot;
    for (int k = 0; k < TEMPL_PER_WORD; k++) {        /* an empty slot, else the oldest */
        if (slot[k].len == 0) { dst = &slot[k]; break; }
        if ((uint16_t)(g_store.age_ctr - slot[k].age) > (uint16_t)(g_store.age_ctr - dst->age))
            dst = &slot[k];
    }
    memset(dst, 0, sizeof *dst);
    dtw_normalize(w, dst->f);
    dst->len = w->n;
    dst->age = g_store.age_ctr++;
}

int dtw_words_enrolled(void)
{
    int n = 0;
    for (int w = 0; w < VOCAB_SIZE; w++)
        for (int k = 0; k < TEMPL_PER_WORD; k++)
            if (g_store.t[w * TEMPL_PER_WORD + k].len) { n++; break; }
    return n;
}

void dtw_match(const word_t *w, match_t *m)
{
    static int8_t x[EP_MAX_FRAMES][NBANDS];
    uint32_t per_word[VOCAB_SIZE];

    dtw_normalize(w, x);
    uint32_t best = DTW_INF, second = DTW_INF;
    int bw = -1, sw = -1;

    for (int v = 0; v < VOCAB_SIZE; v++) {
        per_word[v] = DTW_INF;
        for (int k = 0; k < TEMPL_PER_WORD; k++) {
            const templ_t *t = &g_store.t[v * TEMPL_PER_WORD + k];
            if (!t->len) continue;
            /* anything worse than the current runner-up cannot change the answer */
            uint32_t d = dtw_distance((const int8_t (*)[NBANDS])x, w->n, t->f, t->len, second);
            if (d < per_word[v]) per_word[v] = d;
        }
        if (per_word[v] < best)        { second = best; sw = bw; best = per_word[v]; bw = v; }
        else if (per_word[v] < second) { second = per_word[v]; sw = v; }
    }

    m->raw_word = bw;
    m->word2 = sw;
    m->dist  = best   > 0xFFFF ? 0xFFFF : (uint16_t)best;
    m->dist2 = second > 0xFFFF ? 0xFFFF : (uint16_t)second;
    m->word = bw;
    if (bw < 0 || best > g_store.reject_thr) m->word = -1;
    else if (DTW_MARGIN_PCT && sw >= 0 && second * 100u < best * (100u + DTW_MARGIN_PCT)) m->word = -1;
}

/* Reject threshold from the templates themselves: how far apart are different
 * words for this speaker, this microphone, this room? */
static void auto_threshold(void)
{
    uint32_t sum = 0, cnt = 0;
    for (int i = 0; i < NUM_TEMPL; i++) {
        const templ_t *a = &g_store.t[i];
        if (!a->len) continue;
        uint32_t nearest = DTW_INF;
        for (int j = 0; j < NUM_TEMPL; j++) {
            const templ_t *b = &g_store.t[j];
            if (!b->len || i / TEMPL_PER_WORD == j / TEMPL_PER_WORD) continue;
            uint32_t d = dtw_distance(a->f, a->len, b->f, b->len, nearest);
            if (d < nearest) nearest = d;
        }
        if (nearest < DTW_INF) { sum += nearest; cnt++; }
    }
    g_store.reject_thr = cnt ? (uint16_t)((sum / cnt) * DTW_REJECT_PCT / 100u) : DTW_DEFAULT_THR;
}

bool dtw_store_save(void)
{
    auto_threshold();
    g_store.magic = STORE_MAGIC;
    g_store.checksum = store_sum();
    return hal_store_write(&g_store, sizeof g_store);
}
