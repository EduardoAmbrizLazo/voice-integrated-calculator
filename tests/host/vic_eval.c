/* Accuracy test for the firmware's recognizer (Eduardo's modules in src/mcu):
 *   frontend.c -> wordcap.c -> ceps.c -> dtw.c / recog.c
 *
 *   vic_eval DIR [--k 2.0] [--min 90] [--verbose]
 *
 * DIR holds <word>_<n>.wav files (or one folder of them per speaker).  Each
 * file is resampled to the mic's 62.5 kHz and fed through the same code that
 * runs on the board.  For each take number n, take n of every word is tested
 * against templates made from the other takes, with the reject threshold
 * computed from those templates exactly as enroll.c does after teaching.
 * Also reports how many takes enroll.c's "sounds like the earlier ones?"
 * check (ENR_REPEAT_LIMIT) would have refused while teaching. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "frontend.h"
#include "wordcap.h"
#include "ceps.h"
#include "dtw.h"
#include "recog.h"
#include "enroll.h"
#include "wav.h"

#define MAXTAKES 32
#define NW 19
static const char *const names[NW] = {
    "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine",
    "plus", "minus", "multiply", "equals", "clear", "clock", "timer", "start", "stop"
};

typedef struct { int n; uint8_t f[WC_MAXF][FE_BANDS]; int8_t c[WC_MAXF][CEP_DIM]; } word_t;
static word_t words[NW][MAXTAKES];
static int    have[NW][MAXTAKES];               /* 1 = word found, -1 = file but no word */
static int    verbose;

/* Feed a recording through frontend + wordcap; keep the longest word. */
static int clip_word(const char *path, word_t *out, int *nfound, int *nshort, int *nlong)
{
    int n;
    int32_t *x = wav_load_rate(path, 62500, 20000.0, 32768.0, &n);
    if (!x) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    fe_init();
    wc_init();
    out->n = 0;
    *nfound = *nshort = *nlong = 0;
    for (int i = 0; i < n + 62500 / 2; i++) {          /* + 0.5 s of silence at the end */
        fe_frame_t fr;
        if (!fe_push((int16_t)(i < n ? x[i] : 0), &fr)) continue;
        int r = wc_feed(&fr);
        if (r == WC_SHORT) (*nshort)++;
        if (r == WC_LONG) (*nlong)++;
        if (r != WC_WORD) continue;
        (*nfound)++;
        if (wc_word.n > out->n) {
            out->n = wc_word.n;
            memcpy(out->f, wc_word.f, sizeof out->f);
        }
    }
    free(x);
    if (out->n) ceps_word((const uint8_t (*)[FE_BANDS])out->f, out->n, out->c);
    return out->n > 0;
}

static int word_index(const char *s, size_t len)
{
    for (int w = 0; w < NW; w++)
        if (strlen(names[w]) == len && !strncmp(names[w], s, len)) return w;
    return -1;
}

static int load_speaker(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int files = 0;
    memset(have, 0, sizeof have);
    if (!d) return 0;
    while ((e = readdir(d))) {
        const char *us = strrchr(e->d_name, '_'), *dot = strrchr(e->d_name, '.');
        if (!us || !dot || strcmp(dot, ".wav")) continue;
        int w = word_index(e->d_name, (size_t)(us - e->d_name));
        int k = atoi(us + 1);
        if (w < 0 || k < 0 || k >= MAXTAKES) continue;
        char path[1024];
        int nf, ns, nl;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        files++;
        if (clip_word(path, &words[w][k], &nf, &ns, &nl)) {
            have[w][k] = 1;
            if (verbose || nf > 1)
                printf("  %-14s %2d frames%s\n", e->d_name, words[w][k].n,
                       nf > 1 ? "  [more than one sound; kept the longest]" : "");
        } else {
            have[w][k] = -1;
            printf("  %-14s NO WORD FOUND (%d too short, %d too long)\n", e->d_name, ns, nl);
        }
    }
    closedir(d);
    return files;
}

static int tot_n, tot_ok, tot_rej, tot_ok_norej, refused, checked;
static int conf[NW][NW];

static void evaluate(int k_x10)
{
    static dtw_seq_t seq[NW * MAXTAKES];
    static uint8_t   lab[NW * MAXTAKES];
    int maxk = 0;
    for (int w = 0; w < NW; w++)
        for (int k = 0; k < MAXTAKES; k++) if (have[w][k] && k + 1 > maxk) maxk = k + 1;

    /* enroll.c's repeat check, takes in recording order */
    for (int w = 0; w < NW; w++)
        for (int k = 1; k < maxk; k++) {
            if (have[w][k] != 1) continue;
            int32_t best = DTW_INF;
            dtw_seq_t a = { (const int8_t (*)[CEP_DIM])words[w][k].c, words[w][k].n };
            for (int j = 0; j < k; j++) {
                if (have[w][j] != 1) continue;
                dtw_seq_t b = { (const int8_t (*)[CEP_DIM])words[w][j].c, words[w][j].n };
                int32_t d = dtw_dist(&a, &b);
                if (d < best) best = d;
            }
            if (best == DTW_INF) continue;
            checked++;
            if (best > ENR_REPEAT_LIMIT) {
                refused++;
                printf("  teaching would refuse %s take %d: %.2f dB from earlier takes (limit %.2f)\n",
                       names[w], k, best / DTW_UNITS_PER_DB, ENR_REPEAT_LIMIT / DTW_UNITS_PER_DB);
            }
        }

    for (int test = 0; test < maxk; test++) {
        int nt = 0;
        for (int w = 0; w < NW; w++)
            for (int k = 0; k < maxk; k++)
                if (k != test && have[w][k] == 1) {
                    seq[nt].f = (const int8_t (*)[CEP_DIM])words[w][k].c;
                    seq[nt].n = words[w][k].n;
                    lab[nt++] = (uint8_t)w;
                }
        int32_t limit = rec_threshold(seq, lab, nt, k_x10);
        if (verbose) printf("  test take %d: %d templates, limit %.2f dB\n", test, nt, limit / DTW_UNITS_PER_DB);

        for (int w = 0; w < NW; w++) {
            if (!have[w][test]) continue;
            tot_n++;
            if (have[w][test] < 0) { tot_rej++; continue; }
            dtw_seq_t q = { (const int8_t (*)[CEP_DIM])words[w][test].c, words[w][test].n };
            int32_t dist;
            int best = rec_classify(&q, seq, lab, nt, &dist);
            int got = (best < 0 || dist > limit) ? -1 : best;
            if (best == w) tot_ok_norej++;
            if (got < 0) tot_rej++;
            else { conf[w][got]++; if (got == w) tot_ok++; }
            if (verbose || got != w)
                printf("  %-8s take %d -> %-8s %5.2f dB (limit %.2f)%s\n", names[w], test,
                       best >= 0 ? names[best] : "-", dist >= DTW_INF ? 0 : dist / DTW_UNITS_PER_DB,
                       limit / DTW_UNITS_PER_DB, got < 0 ? "  REJECTED" : got != w ? "  WRONG" : "");
        }
    }
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    double k = REC_K_X10 / 10.0, min_acc = 90.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--k") && i + 1 < argc) k = atof(argv[++i]);
        else if (!strcmp(argv[i], "--min") && i + 1 < argc) min_acc = atof(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = 1;
        else dir = argv[i];
    }
    if (!dir) { fprintf(stderr, "usage: vic_eval DIR [--k 2.0] [--min 90] [--verbose]\n"); return 2; }

    int speakers = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    char sub[1024];
    if (!d) { perror(dir); return 2; }
    while ((e = readdir(d))) {
        struct stat sb;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (stat(sub, &sb) || !S_ISDIR(sb.st_mode)) continue;
        printf("speaker %s\n", e->d_name);
        if (load_speaker(sub)) { evaluate((int)(k * 10 + 0.5)); speakers++; }
    }
    closedir(d);
    if (!speakers) {
        printf("speaker %s\n", dir);
        if (load_speaker(dir)) { evaluate((int)(k * 10 + 0.5)); speakers++; }
    }
    if (!tot_n) { fprintf(stderr, "no <word>_<n>.wav files found\n"); return 1; }

    double acc = 100.0 * tot_ok / tot_n;
    printf("\nFIRMWARE RECOGNIZER (frontend + wordcap + ceps + dtw), %d test words, k = %.1f\n", tot_n, k);
    printf("  word accuracy        : %5.1f%%  (%d/%d, rejections count as errors)\n", acc, tot_ok, tot_n);
    printf("  without rejection    : %5.1f%%\n", 100.0 * tot_ok_norej / tot_n);
    printf("  rejected             : %5.1f%%\n", 100.0 * tot_rej / tot_n);
    printf("  teaching refusals    : %d of %d repeat takes\n", refused, checked);
    printf("  confusions           :");
    int any = 0;
    for (int a = 0; a < NW; a++)
        for (int b = 0; b < NW; b++)
            if (a != b && conf[a][b]) { printf(" %s->%s x%d", names[a], names[b], conf[a][b]); any = 1; }
    printf("%s\n", any ? "" : " none");
    printf("\n%s: accuracy %.1f%% %s %.0f%% target\n", acc >= min_acc ? "PASS" : "FAIL", acc,
           acc >= min_acc ? "meets" : "below", min_acc);
    return acc >= min_acc ? 0 : 1;
}
