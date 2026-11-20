/* Accuracy test for the C recognizer on recorded WAV files.
 *
 *   rec_eval DIR [--k 3.0] [--verbose]
 *
 * DIR holds <word>_<n>.wav files, or one folder of them per speaker (the
 * layout of src/proofofconcept/data).  For each speaker and each take number
 * n, take n of every word is tested against templates made from the other
 * takes (at most REC_TPW of them), and the threshold is computed from those
 * templates exactly as the MCU does after calibration. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "recognizer.h"
#include "wav.h"

#define MAXTAKES 32

static rec_tmpl_t feats[REC_NWORDS][MAXTAKES];
static int        have[REC_NWORDS][MAXTAKES];
static int        verbose;

/* Run a whole clip through the recognizer; keep the longest word found
 * (extract_word() in dtw_poc.py does the same). */
static int clip_features(const char *path, rec_tmpl_t *out, int *nfound)
{
    int n;
    int32_t *x = wav_load(path, &n);
    if (!x) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    static const int32_t zeros[REC_FRAME * 40];
    rec_tmpl_t w;
    int found = 0;
    rec_reset();
    for (int i = 0; i < n; i += 128) {
        rec_push(x + i, n - i < 128 ? n - i : 128);
        if (rec_poll(&w)) { if (!found || w.len > out->len) *out = w; found++; }
    }
    rec_push(zeros, REC_FRAME * 40);            /* silence ends a word cut off by the file end */
    if (rec_poll(&w)) { if (!found || w.len > out->len) *out = w; found++; }
    free(x);
    *nfound = found;
    return found > 0;
}

static int word_index(const char *s, size_t len)
{
    for (int w = 0; w < REC_NWORDS; w++)
        if (strlen(rec_word_name[w]) == len && !strncmp(rec_word_name[w], s, len)) return w;
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
        int nf;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        files++;
        if (clip_features(path, &feats[w][k], &nf)) {
            have[w][k] = 1;
            if (verbose || nf > 1)
                printf("  %-14s %2d frames (%3d ms)%s\n", e->d_name, feats[w][k].len,
                       feats[w][k].len * 16, nf > 1 ? "  [more than one sound; kept the longest]" : "");
        } else {
            have[w][k] = -1;
            printf("  %-14s NO WORD FOUND\n", e->d_name);
        }
    }
    closedir(d);
    return files;
}

static int conf[REC_NWORDS][REC_NWORDS + 1];
static int tot_n, tot_ok, tot_rej, tot_ok_norej;

static void evaluate(int k_q4)
{
    static rec_store_t st;
    int maxk = 0;
    for (int w = 0; w < REC_NWORDS; w++)
        for (int k = 0; k < MAXTAKES; k++) if (have[w][k]) maxk = k + 1 > maxk ? k + 1 : maxk;

    for (int test = 0; test < maxk; test++) {
        memset(&st, 0xFF, sizeof st);           /* like erased flash: empty slots */
        for (int w = 0; w < REC_NWORDS; w++) {
            int s = 0;
            for (int k = 0; k < maxk && s < REC_TPW; k++)
                if (k != test && have[w][k] == 1) st.t[w][s++] = feats[w][k];
        }
        st.magic = REC_STORE_MAGIC; st.tpw = REC_TPW;
        st.thr_q8 = rec_threshold(&st, k_q4);
        if (verbose) printf("  test take %d: threshold %.2f dB\n", test, st.thr_q8 / 256.0 * 0.376);

        for (int w = 0; w < REC_NWORDS; w++) {
            if (!have[w][test]) continue;
            tot_n++;
            if (have[w][test] < 0) { conf[w][REC_NWORDS]++; tot_rej++; continue; }
            rec_result_t r;
            rec_classify(&feats[w][test], &st, &r);
            if (r.best == w) tot_ok_norej++;
            if (r.word < 0) { conf[w][REC_NWORDS]++; tot_rej++; }
            else { conf[w][r.word]++; if (r.word == w) tot_ok++; }
            if (verbose || r.word != w)
                printf("  %-8s take %d -> %-8s %5.2f dB  (next %-8s %5.2f dB)%s\n",
                       rec_word_name[w], test, r.best >= 0 ? rec_word_name[r.best] : "-",
                       r.dist / 256.0 * 0.376, r.second >= 0 ? rec_word_name[r.second] : "-",
                       r.dist2 == REC_INF ? 0 : r.dist2 / 256.0 * 0.376,
                       r.word < 0 ? "  REJECTED" : r.word != w ? "  WRONG" : "");
        }
    }
}

/* Known-answer checks of the DTW, same as selftest() in dtw_poc.py.  Returns
 * the number of failures. */
static int dtw_selftest(void)
{
    static rec_tmpl_t a, slow, other, tiny;
    uint32_t seed = 12345;
    int fails = 0;
    a.len = 30; other.len = 39; slow.len = 39; tiny.len = 10;
    for (int i = 0; i < 39; i++)
        for (int k = 0; k < REC_NB; k++) {
            seed = seed * 1103515245u + 12345u;
            int8_t v = (int8_t)((seed >> 16) % 41) - 20;
            if (i < 30) a.f[i][k] = v;
            seed = seed * 1103515245u + 12345u;
            other.f[i][k] = (int8_t)((int)((seed >> 16) % 41) - 20);
        }
    for (int i = 0; i < 39; i++) memcpy(slow.f[i], a.f[i * 29 / 38], REC_NB);   /* 1.3x slower copy */

    uint32_t d_same = rec_dist(&a, &a), d_slow = rec_dist(&a, &slow), d_other = rec_dist(&a, &other);
    uint32_t d_tiny = rec_dist(&a, &tiny);
#define CHECK(c, msg) do { if (c) printf("  pass  %s\n", msg); else { printf("  FAIL  %s\n", msg); fails++; } } while (0)
    printf("DTW self-test\n");
    CHECK(d_same == 0, "identical words: distance 0");
    CHECK(d_slow * 2 < d_other, "slowed-down copy is much closer than an unrelated word");
    CHECK(d_tiny == REC_INF, "lengths more than 2x apart: no match");
    CHECK(rec_dist(&a, &slow) == rec_dist(&slow, &a), "distance is symmetric");
#undef CHECK
    return fails;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    double k = 3.0, min_acc = 90.0;              /* proposal target: 90% word accuracy */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--k") && i + 1 < argc) k = atof(argv[++i]);
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "--on") && i + 1 < argc) rec_cfg.on_margin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--off") && i + 1 < argc) rec_cfg.off_margin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quiet") && i + 1 < argc) rec_cfg.quiet_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--preroll") && i + 1 < argc) rec_cfg.preroll = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--min") && i + 1 < argc) min_acc = atof(argv[++i]);
        else dir = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: rec_eval DIR [--k 3.0] [--min 90] [--verbose] [--on 27] [--off 21] [--quiet 10] [--preroll 3]\n");
        return 2;
    }
    rec_init();
    int fails = dtw_selftest();

    int speakers = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) { perror(dir); return 2; }
    char sub[1024];
    while ((e = readdir(d))) {
        struct stat sb;
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (stat(sub, &sb) || !S_ISDIR(sb.st_mode)) continue;
        printf("speaker %s\n", e->d_name);
        if (load_speaker(sub)) { evaluate((int)(k * 16 + 0.5)); speakers++; }
    }
    closedir(d);
    if (!speakers) {                            /* files directly in DIR */
        printf("speaker %s\n", dir);
        if (load_speaker(dir)) { evaluate((int)(k * 16 + 0.5)); speakers++; }
    }
    if (!tot_n) { fprintf(stderr, "no <word>_<n>.wav files found\n"); return 1; }

    printf("\nC RECOGNIZER, %d speaker(s), %d test words, up to %d templates per word\n",
           speakers, tot_n, REC_TPW);
    printf("  word accuracy        : %5.1f%%  (%d/%d, rejections count as errors)\n",
           100.0 * tot_ok / tot_n, tot_ok, tot_n);
    printf("  without rejection    : %5.1f%%\n", 100.0 * tot_ok_norej / tot_n);
    printf("  rejected             : %5.1f%%\n", 100.0 * tot_rej / tot_n);
    printf("  confusions           :");
    int any = 0;
    for (int a = 0; a < REC_NWORDS; a++)
        for (int b = 0; b < REC_NWORDS; b++)
            if (a != b && conf[a][b]) { printf(" %s->%s x%d", rec_word_name[a], rec_word_name[b], conf[a][b]); any = 1; }
    printf("%s\n", any ? "" : " none");

    double acc = 100.0 * tot_ok / tot_n;
    if (acc < min_acc) fails++;
    printf("\n%s: DTW self-test %s, accuracy %.1f%% %s %.0f%% target\n", fails ? "FAIL" : "PASS",
           fails > (acc < min_acc) ? "failed" : "passed", acc, acc < min_acc ? "below" : "meets", min_acc);
    return fails ? 1 : 0;
}
