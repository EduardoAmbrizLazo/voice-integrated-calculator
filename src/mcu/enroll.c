/* enroll.c - see enroll.h */
#include "enroll.h"
#include "ceps.h"
#include "dtw.h"
#include "recog.h"

/* ------------------------------------------------------------------ vocabulary
 * The words to teach, in this order. Keep each one at 10 letters or fewer. To add "divide" (it is
 * on the design-review slide, but the test recordings do not have it), add "DIVIDE" to the list. */
static const char *const vocab[] = {
    "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE",
    "PLUS", "MINUS", "MULTIPLY", "EQUALS", "CLEAR", "CLOCK", "TIMER", "START", "STOP",
};
#define NWORDS ((int)(sizeof vocab / sizeof vocab[0]))
typedef char vocab_fits_in_tables[(NWORDS <= ENR_MAX_WORDS) ? 1 : -1];     /* compile error if too many */
typedef char needs_two_samples[(ENR_SAMPLES >= 2) ? 1 : -1];               /* the threshold compares recordings of one word */

/* ------------------------------------------------------------------ storage */
#define MAXT (ENR_MAX_WORDS * ENR_SAMPLES)

static int8_t   pool[ENR_POOL_FRAMES][CEP_DIM];     /* every stored recording, one after another */
static int      pool_used;
static dtw_seq_t seq[MAXT];                         /* the stored recordings...                  */
static uint8_t  label[MAXT];                        /* ...and which word each one is             */
static int      nt;

static int      state;
static int      cur_word, cur_sample, fails;
static int      word_t0, word_pool0;                /* where the current word's recordings start */
static int32_t  limit;                              /* reject threshold, set when teaching ends  */

void enr_abort(void)
{
    state = ENR_IDLE;
    nt = pool_used = 0;
    cur_word = cur_sample = fails = word_t0 = word_pool0 = 0;
    limit = 0;
}

void enr_begin(void)
{
    enr_abort();
    state = ENR_TEACHING;
}

int  enr_state(void)       { return state; }
int  enr_word(void)        { return cur_word; }
int  enr_sample(void)      { return cur_sample; }
int  enr_done(void)        { return nt; }
int  enr_total(void)       { return NWORDS * ENR_SAMPLES; }
int  enr_word_count(void)  { return NWORDS; }
int  enr_pool_used(void)   { return pool_used; }
const char *enr_name(int w) { return (w >= 0 && w < NWORDS) ? vocab[w] : "?"; }

/* ----------------------------------------------------------------- teaching */
int enr_submit(const uint8_t (*bands)[FE_BANDS], int n, int32_t *dist)
{
    *dist = -1;
    if (state != ENR_TEACHING) return ENR_IGNORED;
    if (n < 1 || n > DTW_MAXLEN) return ENR_TOO_DIFFERENT;
    if (pool_used + n > ENR_POOL_FRAMES) return ENR_NO_MEMORY;

    int8_t (*dst)[CEP_DIM] = &pool[pool_used];
    ceps_word(bands, n, dst);                       /* tentative: only kept if accepted below   */
    dtw_seq_t s = { (const int8_t (*)[CEP_DIM])dst, n };

    /* it must sound like at least one earlier recording of this word */
    int32_t nearest = DTW_INF;
    for (int t = word_t0; t < nt; t++) {
        int32_t d = dtw_dist(&s, &seq[t]);
        if (d < nearest) nearest = d;
    }
    int result = -1;
    if (nt > word_t0) {                             /* there is something to compare with        */
        *dist = nearest >= DTW_INF ? -1 : nearest;
        if (nearest > ENR_REPEAT_LIMIT) {
            if (++fails < 2) return ENR_TOO_DIFFERENT;
            /* refused twice running: the earlier recordings are probably the odd ones out, so
             * forget them and start this word again from this recording */
            nt = word_t0;
            pool_used = word_pool0;
            cur_sample = 0;
            fails = 0;
            dst = &pool[pool_used];
            ceps_word(bands, n, dst);
            s.f = (const int8_t (*)[CEP_DIM])dst;
            result = ENR_RESTARTED;
        }
    }

    seq[nt] = s;
    label[nt] = (uint8_t)cur_word;
    nt++;
    pool_used += n;
    fails = 0;
    cur_sample++;
    if (result == ENR_RESTARTED) return result;

    if (cur_sample < ENR_SAMPLES) return ENR_GOT_IT;
    cur_word++; cur_sample = 0;
    word_t0 = nt; word_pool0 = pool_used;
    if (cur_word < NWORDS) return ENR_WORD_DONE;

    limit = rec_threshold(seq, label, nt, REC_K_X10);   /* how far apart are two recordings of one word? */
    state = ENR_READY;
    return ENR_FINISHED;
}

/* -------------------------------------------------------------- recognition */
int enr_recognize(const uint8_t (*bands)[FE_BANDS], int n, int *best, int32_t *dist, int32_t *lim)
{
    *best = -1; *dist = DTW_INF; *lim = limit;
    if (state != ENR_READY || n < 1 || n > DTW_MAXLEN) return -1;

    static int8_t probe[DTW_MAXLEN][CEP_DIM];       /* the spoken word, in the same form as the templates (static: keeps the stack small) */
    ceps_word(bands, n, probe);
    dtw_seq_t w = { (const int8_t (*)[CEP_DIM])probe, n };

    *best = rec_classify(&w, seq, label, nt, dist);
    if (*best < 0 || *dist > limit) return -1;
    return *best;
}

/* ------------------------------------------------------------------ saving */
#define SAVE_MAGIC    0x54434956u       /* "VICT" */
#define SAVE_VERSION  1u

typedef struct {
    uint32_t magic, version;
    uint32_t nt, pool_used;             /* recordings, frames of storage they use   */
    int32_t  limit;                     /* reject threshold worked out after teaching */
    uint32_t samples, nwords;           /* must match this build                    */
    uint32_t sum;                       /* checksum of everything after the header  */
} save_hdr_t;
typedef char save_hdr_is_32_bytes[(sizeof(save_hdr_t) == 32) ? 1 : -1];

static uint32_t fnv(uint32_t h, const void *p, uint32_t n)     /* FNV-1a checksum */
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* checksum of: word of each recording, length of each recording, the recordings */
static uint32_t body_sum(const uint8_t *lab, const uint8_t *len, int n, const int8_t *data, int frames)
{
    uint32_t h = 2166136261u;
    h = fnv(h, lab, (uint32_t)n);
    h = fnv(h, len, (uint32_t)n);
    return fnv(h, data, (uint32_t)frames * CEP_DIM);
}

uint32_t enr_save(void (*put)(const void *src, uint32_t n))
{
    if (state != ENR_READY || nt < 1) return 0;
    uint8_t len[MAXT];
    for (int i = 0; i < nt; i++) len[i] = (uint8_t)seq[i].n;
    save_hdr_t h = { SAVE_MAGIC, SAVE_VERSION, (uint32_t)nt, (uint32_t)pool_used, limit,
                     ENR_SAMPLES, (uint32_t)NWORDS, 0 };
    h.sum = body_sum(label, len, nt, &pool[0][0], pool_used);
    put(&h, sizeof h);
    put(label, (uint32_t)nt);
    put(len, (uint32_t)nt);
    put(pool, (uint32_t)pool_used * CEP_DIM);
    return (uint32_t)(sizeof h + 2u * (uint32_t)nt + (uint32_t)pool_used * CEP_DIM);
}

int enr_load(const void *blob)
{
    save_hdr_t h;
    const uint8_t *b = blob;
    for (unsigned i = 0; i < sizeof h; i++) ((uint8_t *)&h)[i] = b[i];
    if (h.magic != SAVE_MAGIC || h.version != SAVE_VERSION) return 0;
    if (h.samples != ENR_SAMPLES || h.nwords != (uint32_t)NWORDS) return 0;
    if (h.nt != (uint32_t)(NWORDS * ENR_SAMPLES) || h.nt > MAXT) return 0;
    if (h.pool_used > ENR_POOL_FRAMES) return 0;

    const uint8_t *lab = b + sizeof h, *len = lab + h.nt;
    const int8_t *data = (const int8_t *)(len + h.nt);
    uint32_t frames = 0;
    for (uint32_t i = 0; i < h.nt; i++) {
        if (lab[i] >= NWORDS || len[i] < 1 || len[i] > DTW_MAXLEN) return 0;
        frames += len[i];
    }
    if (frames != h.pool_used) return 0;
    if (body_sum(lab, len, (int)h.nt, data, (int)h.pool_used) != h.sum) return 0;

    /* all good: copy into RAM and rebuild the list of recordings */
    enr_abort();
    for (uint32_t i = 0; i < h.pool_used * CEP_DIM; i++) (&pool[0][0])[i] = data[i];
    int at = 0;
    for (uint32_t i = 0; i < h.nt; i++) {
        seq[i].f = (const int8_t (*)[CEP_DIM])&pool[at];
        seq[i].n = len[i];
        label[i] = lab[i];
        at += len[i];
    }
    nt = (int)h.nt;
    pool_used = (int)h.pool_used;
    limit = h.limit;
    state = ENR_READY;
    return 1;
}
