/* splash.c - see splash.h */
#include "splash.h"

#define BARS        21          /* 21 bars x 6 pixels wide = 126 of the 128 columns        */
#define BAR_FRAMES  16          /* frames of the waveform, one every 50 ms                 */
#define TYPE_MS     40          /* time per typed letter                                   */
#define FILL_STEPS  20          /* progress bar steps (5 pixels each)                      */
#define FILL_MS     25          /* time per step                                           */
#define HOLD_MS     300         /* pause on the finished splash                            */

/* ---------------------------------------------------------------- drawing helpers */
static void clear(void)
{
    uint8_t b = 0x01;
    splash_send(&b, 1);
}

/* n raw columns starting at column x of a page; each byte is 8 vertical pixels, bit 0 on top */
static void raw(int x, int page, const uint8_t *cols, int n)
{
    uint8_t b[3 + 128];
    if (n > 128 - x) n = 128 - x;
    b[0] = 0x05;
    b[1] = (uint8_t)x;
    b[2] = (uint8_t)page;
    for (int i = 0; i < n; i++) b[3 + i] = cols[i];
    splash_send(b, 3 + n);
}

/* a centred line of text, showing only its first `shown` letters (the rest stay blank, so the
 * line does not shift as letters appear). scale 1..3; the text is at most 128/(6*scale) letters. */
static void text(int page, int scale, const char *s, int shown)
{
    int cols = 128 / (6 * scale), len = 0;
    uint8_t b[3 + 21];
    while (s[len] && len < cols) len++;
    int pad = (cols - len) / 2;
    b[0] = 0x02;
    b[1] = (uint8_t)((128 - cols * 6 * scale) / 2);
    b[2] = (uint8_t)(page | (scale << 4));
    for (int i = 0; i < cols; i++)
        b[3 + i] = (i >= pad && i < pad + len && i - pad < shown) ? (uint8_t)s[i - pad] : ' ';
    splash_send(b, 3 + cols);
}

static void wait_until(uint32_t t)
{
    while ((int32_t)(splash_ms() - t) < 0) { }
}

/* ------------------------------------------------------------------ waveform */
static int tri(int p)                   /* triangle wave, 0..15..0 every 32 */
{
    p &= 31;
    return p < 16 ? p : 31 - p;
}

/* rows lit on a page for a bar reaching h pixels above and below the centre line (y = 32) */
static uint8_t bar_mask(int page, int h)
{
    uint8_t m = 0;
    for (int bit = 0; bit < 8; bit++) {
        int y = page * 8 + bit;
        if (y >= 32 - h && y < 32 + h) m |= (uint8_t)(1u << bit);
    }
    return m;
}

/* one frame of the waveform on pages 2..5; env = how far it has swelled, 0..16 */
static void draw_bars(int frame, int env)
{
    uint8_t cols[4][128];
    for (int p = 0; p < 4; p++)
        for (int x = 0; x < 128; x++) cols[p][x] = 0;
    for (int b = 0; b < BARS; b++) {
        int win = 16 - (b > 10 ? b - 10 : 10 - b);               /* taller in the middle: 6..16 */
        int h = 1 + (tri(b * 3 + frame * 2) * env * win) / (16 * 16);
        if (h > 16) h = 16;
        for (int p = 0; p < 4; p++) {
            uint8_t m = bar_mask(2 + p, h);
            for (int x = 0; x < 4; x++) cols[p][1 + b * 6 + x] = m;     /* 4 wide, 2 gap */
        }
    }
    for (int p = 0; p < 4; p++) raw(0, 2 + p, cols[p], 128);
}

/* ------------------------------------------------------------------ the show */
void splash_run(void)
{
    uint32_t t = splash_ms();

    /* 1. waveform: swell, hold, settle to a flat line */
    clear();
    for (int f = 0; f < BAR_FRAMES; f++) {
        int env = f < 5 ? f * 16 / 5 : f < 10 ? 16 : (BAR_FRAMES - 1 - f) * 16 / 5;
        draw_bars(f, env);
        t += 50;
        wait_until(t);
    }

    /* 2. the name types itself in */
    clear();
    for (int i = 1; i <= 5; i++)  { text(1, 3, "VOICE", i);      t += TYPE_MS; wait_until(t); }
    for (int i = 1; i <= 10; i++) { text(4, 2, "CALCULATOR", i); t += TYPE_MS; wait_until(t); }

    /* 3. progress bar under the title: a thin track, then a thick fill */
    uint8_t track[100], fill[5];
    for (int i = 0; i < 100; i++) track[i] = 0x18;               /* 2 pixels tall  */
    for (int i = 0; i < 5; i++)   fill[i]  = 0x3C;               /* 4 pixels tall  */
    raw(14, 6, track, 100);
    for (int s = 0; s < FILL_STEPS; s++) {
        raw(14 + s * 5, 6, fill, 5);
        t += FILL_MS;
        wait_until(t);
    }
    t += HOLD_MS;
    wait_until(t);
}
