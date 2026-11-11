#ifndef ENDPOINT_H
#define ENDPOINT_H
#include <stdint.h>
#include "config.h"
#include "tables.h"

typedef struct {
    uint8_t n;
    uint8_t f[EP_MAX_FRAMES][NBANDS];
} word_t;

#define EP_HIST  (EP_PREROLL + EP_START_FRAMES)
#define EP_CAP   (EP_MAX_FRAMES + EP_END_FRAMES)

typedef struct {
    uint8_t  state;
    uint8_t  loud_run, quiet_run;
    uint8_t  hist[EP_HIST][NBANDS];      /* last few frames, for the pre-roll */
    uint8_t  hist_n;
    uint8_t  cap[EP_CAP][NBANDS];
    uint8_t  cap_n;
    int32_t  floor_q4;                   /* noise floor, energy counts * 16   */
    uint8_t  have_floor;
} endpoint_t;

#define EP_NONE      0
#define EP_WORD      1      /* *out holds a complete word                       */
#define EP_BAD      -1      /* something loud that was too short or too long    */

void ep_init(endpoint_t *e);
void ep_reset(endpoint_t *e);            /* drop any partial word, keep noise floor */
int  ep_push(endpoint_t *e, const uint8_t feat[NBANDS], uint8_t energy, word_t *out);

#endif
