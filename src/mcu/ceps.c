/* ceps.c - see ceps.h. Integer maths only. */
#include "ceps.h"
#include "fe_tables.h"

void ceps_word(const uint8_t (*bands)[FE_BANDS], int n, int8_t (*out)[CEP_DIM])
{
    int32_t mean16[FE_BANDS];                          /* per-band average of the word, x16 */
    for (int b = 0; b < FE_BANDS; b++) {
        int32_t sum = 0;
        for (int i = 0; i < n; i++) sum += bands[i][b];
        mean16[b] = n ? (sum * 16 + n / 2) / n : 0;
    }
    for (int i = 0; i < n; i++) {
        int32_t dev[FE_BANDS];                         /* band value minus the word's average, x16 */
        for (int b = 0; b < FE_BANDS; b++) dev[b] = 16 * (int32_t)bands[i][b] - mean16[b];
        for (int k = 0; k < CEP_DIM; k++) {
            int32_t acc = 0;                           /* x16 (counts) x 4096 (matrix) */
            //for (int b = 0; b < FE_BANDS; b++) acc += dev[b] * (int32_t)fe_dct[k][b];
            int32_t v = (acc + (1 << 15)) >> 16;       /* back to counts, rounded */
            out[i][k] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
        }
    }
}
