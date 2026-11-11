#ifndef DTW_H
#define DTW_H
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "endpoint.h"

typedef struct {
    uint8_t  len;                          /* 0 = empty slot                    */
    uint8_t  rsvd;
    uint16_t age;                          /* recording order, for replacement  */
    int8_t   f[EP_MAX_FRAMES][NBANDS];     /* mean-normalised features          */
} templ_t;                                 /* 604 bytes                         */

typedef struct {
    uint32_t magic;
    uint32_t checksum;                     /* over everything after this field  */
    uint16_t reject_thr;
    uint16_t age_ctr;
    uint32_t rsvd;
    templ_t  t[NUM_TEMPL];                 /* slot = word * TEMPL_PER_WORD + k  */
} store_t;

typedef struct {
    int      word;        /* best word, or -1 if rejected / nothing enrolled */
    int      raw_word;    /* best word before the reject test                */
    uint16_t dist;        /* its distance (x16 per path step)                */
    int      word2;       /* best different word                             */
    uint16_t dist2;
} match_t;

extern store_t g_store;

bool dtw_store_load(void);                       /* from flash; false = empty      */
bool dtw_store_save(void);                       /* recompute threshold + flash    */
void dtw_store_wipe(void);
void dtw_enroll(int word, const word_t *w);      /* replaces the oldest slot       */
int  dtw_words_enrolled(void);
void dtw_match(const word_t *w, match_t *m);

/* low level, also used by the tests */
void     dtw_normalize(const word_t *w, int8_t out[][NBANDS]);
uint32_t dtw_distance(const int8_t a[][NBANDS], int n, const int8_t b[][NBANDS], int m,
                      uint32_t abandon);         /* returns dist x16, or DTW_INF  */
#define DTW_INF 0x3FFFFFFFu

#endif
