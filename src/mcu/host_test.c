/* host_test.c - runs frontend.c and wordcap.c on a PC.
 *   host_test frames [bypass]   int16 samples on stdin -> 13-byte frames on stdout
 *   host_test words  [bypass]   int16 samples on stdin -> text, one block per detected word
 */
#include <stdio.h>
#include <string.h>
#include "frontend.h"
#include "wordcap.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int words = !strcmp(argv[1], "words");
    fe_init();
    wc_init();
    fe_set_bypass(argc > 2 && argv[2][0] == '1');
    int16_t x;
    long frame = 0;
    while (fread(&x, sizeof x, 1, stdin) == 1) {
        fe_frame_t fr;
        if (!fe_push(x, &fr)) continue;
        if (!words) {
            fwrite(fr.band, 1, FE_BANDS, stdout);
            fwrite(&fr.loud, 1, 1, stdout);
        } else {
            int r = wc_feed(&fr);
            if (r == WC_NONE) { frame++; continue; }
            printf("E %d %ld %d %d\n", r, frame, wc_word.n, wc_word_ms());
            if (r == WC_WORD)
                for (int i = 0; i < wc_word.n; i++) {
                    for (int k = 0; k < FE_BANDS; k++) printf("%d ", wc_word.f[i][k]);
                    printf("\n");
                }
        }
        frame++;
    }
    return 0;
}
