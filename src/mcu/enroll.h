/* enroll.h - teach the device your voice, then recognise words
 *
 * Teaching: the device asks for each word in the vocabulary ENR_SAMPLES times. Each recording
 * that wordcap captures goes to enr_submit(); the device checks it sounds like the earlier
 * recordings of the same word, stores it, and moves on. When the last word is done it works out
 * its own reject threshold from the recordings (the same rule as dtw_poc.py) and switches to
 * recognition: enr_recognize() says which word was spoken, or -1 if it is not a known word.
 *
 * The recordings are kept in RAM, so they are lost at power-off (storing them in flash is the
 * next item). No hardware in here, so it can be tested on a PC.
 */
#ifndef ENROLL_H
#define ENROLL_H

#include <stdint.h>
#include "frontend.h"

#ifndef ENR_SAMPLES
#define ENR_SAMPLES        3        /* recordings of each word                                       */
#endif
#define ENR_MAX_WORDS      24       /* room for this many words (the list itself is in enroll.c)      */
#ifndef ENR_POOL_FRAMES
#define ENR_POOL_FRAMES    2400     /* feature frames of storage: 11 bytes each = 26 KB. The 57 test
                                       recordings needed 1827 (33 per word on average, 48 at most).   */
#endif
/* A new recording must be within this distance of an earlier recording of the same word, or it is
 * refused as "too different". Units are 1/16 count: 110 = 2.7 dB. This is only a coarse check for a
 * clearly wrong word. Over 8 noise patterns, two recordings of the same word were at most 2.08 dB
 * apart (median 1.35), but where the detector cuts a word moves around, and distances of 2.2-2.3 dB
 * showed up between honest repeats in a back-to-back run, so the limit leaves room above that.
 * Different words are usually 2.3 dB or more apart, so this catches the obvious slips only. */
#ifndef ENR_REPEAT_LIMIT
#define ENR_REPEAT_LIMIT   110
#endif

enum { ENR_IDLE = 0,        /* nothing taught yet (or aborted)                 */
       ENR_TEACHING,        /* asking for words                                */
       ENR_READY };         /* taught; enr_recognize() works                   */

/* what enr_submit() says about a recording */
enum { ENR_GOT_IT = 0,      /* kept; ask for another recording of this word    */
       ENR_WORD_DONE,       /* kept; that was the last of this word, next word */
       ENR_FINISHED,        /* kept; every word is taught, now in ENR_READY    */
       ENR_TOO_DIFFERENT,   /* refused: does not sound like the earlier ones   */
       ENR_RESTARTED,       /* refused twice in a row, so the earlier recordings of this word were
                               dropped and this one is now recording 1         */
       ENR_NO_MEMORY,       /* the pool is full: teaching cannot continue      */
       ENR_IGNORED };       /* not teaching right now                          */

void enr_begin(void);                   /* throw away everything and start with the first word */
void enr_abort(void);                   /* stop teaching and throw away everything             */

int  enr_state(void);
int  enr_word(void);                    /* word being taught, 0..enr_word_count()-1            */
int  enr_sample(void);                  /* recording of it being asked for, 0..ENR_SAMPLES-1   */
int  enr_done(void);                    /* recordings kept so far                              */
int  enr_total(void);                   /* recordings needed in all                            */
int  enr_word_count(void);
const char *enr_name(int word);         /* upper case, for the display                         */
int  enr_pool_used(void);               /* frames of storage in use                            */

int  enr_submit(const uint8_t (*bands)[FE_BANDS], int n, int32_t *dist);
                                        /* *dist = distance to the nearest earlier recording
                                           of this word, or -1 if this was the first          */

/* Recognise a captured word. Returns the word, or -1 if it is too far from everything taught
 * (or nothing is taught). *best = the nearest word either way (-1 if none), *dist its distance,
 * *limit the reject threshold (all in 1/16 count; 41 = 1 dB). */
int  enr_recognize(const uint8_t (*bands)[FE_BANDS], int n, int *best, int32_t *dist, int32_t *limit);

/* Saving what was taught, so it survives power-off (the caller supplies the flash writing).
 * enr_save: only after teaching is complete (ENR_READY). Calls put() with the bytes to store, in
 *   order: a header with a checksum, each recording's word and length, then the recordings.
 *   Returns the number of bytes, or 0 if there is nothing to save. At most ENR_SAVE_MAX bytes.
 * enr_load: blob = where those bytes were stored. Checks everything (header, sizes, checksum,
 *   same vocabulary and ENR_SAMPLES); if it is all right, loads it and switches to ENR_READY and
 *   returns 1. Otherwise changes nothing and returns 0 (never taught, or a half-finished save). */
#define ENR_SAVE_MAX  (32 + 2 * ENR_MAX_WORDS * ENR_SAMPLES + ENR_POOL_FRAMES * (FE_BANDS - 1))
uint32_t enr_save(void (*put)(const void *src, uint32_t n));
int      enr_load(const void *blob);

#endif
