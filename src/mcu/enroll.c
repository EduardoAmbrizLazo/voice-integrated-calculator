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
