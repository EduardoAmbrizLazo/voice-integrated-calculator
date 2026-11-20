/* VIC firmware: microphone -> 12-band features -> DTW word recognition.
 *
 * Built from midpointmain.c (same clock, SPI, RTC and microphone code).  The
 * loudness-only word detector is replaced by src/mcu/recognizer.c, which
 * computes 12 band energies per 16 ms frame, cuts out each word and matches
 * it against stored templates with DTW.
 *
 * SEGGER project: add this file and src/mcu/recognizer.c; add src/mcu to the
 * include directories.
 *
 * USING IT
 *   First power-up: the screen says NOT RECORDED.  Press PA0 twice to start.
 *   Recording: the screen shows a word; say it, wait for the next one.  The 19
 *   words come up 3 times (57 templates).  PA1 = redo the previous word.
 *   Templates are saved in flash as you go and survive power-off.
 *   Listening: say a word; the screen shows what it heard, and the match
 *   distance (smaller = closer).  "??" means it was rejected as unknown.
 *   Press PA0 twice at any time to record everything again.
 *
 * UART (115200 on the ST-Link USB port) logs every word with its distances.
 */

#include <stddef.h>
#include <stdint.h>
#include "stm32l432xx.h"
#include "recognizer.h"

#define STEP 2          /* 1 = no microphone: just prove MCU -> FPGA -> OLED (shows HELLO)
                           2 = recognizer                                                 */
#define I2S_SLOT    0   /* 0 = mic SEL tied to GND, 1 = SEL tied to 3V3                   */
#define THRESH_K_Q4 48  /* reject threshold = mean + 3.0 x std of same-word distances     */

/* Templates (57 x 608 bytes = 34.7 KB) live in the last 36 KB of flash
 * (pages 110..127).  The program must stay below this address (220 KB):
 * check the size after building. */
#define STORE_ADDR  0x08037000u
#define STORE_PAGES 18u
#define PAGE_SIZE   2048u
_Static_assert(sizeof(rec_store_t) <= STORE_PAGES * PAGE_SIZE, "template store too big for flash area");
#define store ((const rec_store_t *)STORE_ADDR)

/* ------------------------------------------------------------ time, serial */
static volatile uint32_t g_ms;
void SysTick_Handler(void) { g_ms++; }

static void log_str(const char *s)
{
    while (*s) {
        while (!(USART2->ISR & USART_ISR_TXE)) { }
        USART2->TDR = (uint8_t)*s++;
    }
}

/* ------------------------------------------------------ display (via FPGA) */
static void fpga_send(const uint8_t *buf, int n)        /* one SPI packet, CS framed */
{
    GPIOB->BSRR = 1u << (4 + 16);
    for (int i = 0; i < n; i++) {
        *(volatile uint8_t *)&SPI3->DR = buf[i];
        while (!(SPI3->SR & SPI_SR_RXNE)) { }
        (void)*(volatile uint8_t *)&SPI3->DR;
    }
    GPIOB->BSRR = 1u << 4;
    for (volatile int d = 0; d < 200; d++) { }
}

static void disp_clear(void) { uint8_t b = 0x01; fpga_send(&b, 1); }

/* Write one full row of text.  page = 0..7 (8 pixels each), scale = 1..3. */
static void disp_line(int page, int scale, const char *s, int centre)
{
    int cols = 128 / (6 * scale), len = 0, pad;
    uint8_t b[3 + 21];
    while (s[len] && len < cols) len++;
    pad = centre ? (cols - len) / 2 : 0;
    b[0] = 0x02;                                        /* "draw text" command    */
    b[1] = (uint8_t)((128 - cols * 6 * scale) / 2);     /* x position             */
    b[2] = (uint8_t)(page | (scale << 4));
    for (int i = 0; i < cols; i++) b[3 + i] = (i >= pad && i < pad + len) ? (uint8_t)s[i - pad] : ' ';
    fpga_send(b, 3 + cols);
}

/* A word in big letters on pages 2..4: scale 3 fits 7 letters, scale 2 fits 10. */
static void disp_big(const char *s)
{
    int len = 0;
    while (s[len]) len++;
    if (len <= 7) disp_line(2, 3, s, 1);
    else { disp_line(2, 1, "", 0); disp_line(3, 2, s, 1); }
}

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_int(char *p, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0; return p;
}

static char *put_2d(char *p, unsigned v)             /* two digits, e.g. 07 */
{
    *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p;
}

/* DTW distance (Q8 counts of 0.376 dB) as dB with one decimal, e.g. "2.4" */
static char *put_db(char *p, uint32_t d_q8)
{
    if (d_q8 == REC_INF) return put_str(p, "-");
    uint32_t t = (d_q8 * 94u + 3200u) / 6400u;          /* tenths of a dB */
    p = put_int(p, t / 10); *p++ = '.'; *p++ = (char)('0' + t % 10); *p = 0;
    return p;
}

/* ---------------------------------------------------------- real-time clock */
/* The RTC runs from the 32.768 kHz watch crystal (LSE) and keeps time through
 * a reset; it restarts from the build time after power is removed. */
static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_set(unsigned h, unsigned m, unsigned s)
{
    int lsi = (RCC->BDCR & RCC_BDCR_RTCSEL) == RCC_BDCR_RTCSEL_1;
    RTC->WPR = 0xCA; RTC->WPR = 0x53;                   /* unlock the RTC registers      */
    RTC->ISR |= RTC_ISR_INIT;                           /* stop the calendar to load it  */
    while (!(RTC->ISR & RTC_ISR_INITF)) { }
    RTC->PRER = (127u << 16) | (lsi ? 249u : 255u);     /* divide the clock down to 1 Hz */
    RTC->CR &= ~RTC_CR_FMT;                             /* 24-hour format                */
    RTC->TR = ((uint32_t)bcd(h) << 16) | ((uint32_t)bcd(m) << 8) | bcd(s);
    RTC->DR = 0x00262101;                               /* any valid date                */
    RTC->ISR &= ~RTC_ISR_INIT;                          /* start counting                */
    RTC->WPR = 0xFF;                                    /* lock again                    */
}

static void rtc_get(unsigned *h, unsigned *m, unsigned *s)
{
    uint32_t tr = RTC->TR;                              /* time is stored as BCD digits  */
    (void)RTC->DR;                                      /* reading the date releases the snapshot */
    *h = ((tr >> 20) & 3) * 10 + ((tr >> 16) & 15);
    *m = ((tr >> 12) & 7) * 10 + ((tr >> 8) & 15);
    *s = ((tr >> 4) & 7) * 10 + (tr & 15);
}

static void rtc_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN | RCC_APB1ENR1_RTCAPBEN;
    PWR->CR1 |= PWR_CR1_DBP;                            /* allow writes to the RTC domain */
    if ((RCC->BDCR & RCC_BDCR_RTCEN) && (RTC->ISR & RTC_ISR_INITS)) return;    /* already running */

    RCC->BDCR |= RCC_BDCR_BDRST; RCC->BDCR &= ~RCC_BDCR_BDRST;
    RCC->BDCR |= RCC_BDCR_LSEON;                        /* start the watch crystal        */
    uint32_t t0 = g_ms;
    while (!(RCC->BDCR & RCC_BDCR_LSERDY) && g_ms - t0 < 3000) { }
    if (RCC->BDCR & RCC_BDCR_LSERDY) {
        RCC->BDCR |= RCC_BDCR_RTCSEL_0;                 /* RTC runs from the crystal      */
    } else {                                            /* no crystal: internal 32 kHz    */
        RCC->CSR |= RCC_CSR_LSION;
        while (!(RCC->CSR & RCC_CSR_LSIRDY)) { }
        RCC->BDCR |= RCC_BDCR_RTCSEL_1;
    }
    RCC->BDCR |= RCC_BDCR_RTCEN;
    const char *t = __TIME__;                           /* "hh:mm:ss" when this file was compiled */
    rtc_set((unsigned)((t[0] - '0') * 10 + t[1] - '0'), (unsigned)((t[3] - '0') * 10 + t[4] - '0'),
            (unsigned)((t[6] - '0') * 10 + t[7] - '0'));
}

/* Top row: what mode we are in, and HH:MM:SS. */
static char     title[13] = "VIC";
static unsigned shown_sec = 99;
static void draw_clock(int force)
{
    unsigned h, m, sec;
    char b[24], *p;
    rtc_get(&h, &m, &sec);
    if (sec == shown_sec && !force) return;
    shown_sec = sec;
    p = put_str(b, title);
    while (p < b + 13) *p++ = ' ';
    p = put_2d(p, h); *p++ = ':'; p = put_2d(p, m); *p++ = ':'; put_2d(p, sec);
    disp_line(0, 1, b, 0);
}

/* -------------------------------------------------------------- flash store */
/* Erase / program the template area.  Doubleword (8-byte) programming only
 * works on erased flash, so a write reads the whole 2 KB page into RAM,
 * changes it there, erases the page and programs it back. */
static uint64_t page_buf[PAGE_SIZE / 8];

static void flash_wait(void) { while (FLASH->SR & FLASH_SR_BSY) { } }

static void flash_unlock(void)
{
    flash_wait();
    if (FLASH->CR & FLASH_CR_LOCK) { FLASH->KEYR = 0x45670123u; FLASH->KEYR = 0xCDEF89ABu; }
    FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR |
                FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR;
}

static void flash_lock_and_flush(void)
{
    FLASH->CR |= FLASH_CR_LOCK;
    FLASH->ACR &= ~FLASH_ACR_DCEN;                      /* drop stale cached reads */
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
}

static int flash_erase_page(uint32_t addr)
{
    uint32_t page = (addr - FLASH_BASE) / PAGE_SIZE;
    FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_PG)) | FLASH_CR_PER | (page << FLASH_CR_PNB_Pos);
    FLASH->CR |= FLASH_CR_STRT;
    flash_wait();
    FLASH->CR &= ~FLASH_CR_PER;
    return !(FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_OPERR));
}

static int flash_program_page(uint32_t addr, const uint64_t *src)
{
    int ok = 1;
    FLASH->CR |= FLASH_CR_PG;
    for (uint32_t i = 0; i < PAGE_SIZE / 8 && ok; i++) {
        if (src[i] == ~0ull) continue;                  /* already erased */
        volatile uint32_t *dst = (volatile uint32_t *)(addr + 8 * i);
        dst[0] = (uint32_t)src[i];
        dst[1] = (uint32_t)(src[i] >> 32);
        flash_wait();
        if (FLASH->SR & (FLASH_SR_PROGERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR))
            ok = 0;
        FLASH->SR = FLASH_SR_EOP;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    return ok;
}

/* The audio interrupt is paused while flash is busy (the CPU stalls anyway),
 * then the recognizer restarts cleanly. */
static void audio_pause(void)  { NVIC_DisableIRQ(DMA2_Channel1_IRQn); }
static void audio_resume(void) { rec_reset(); NVIC_EnableIRQ(DMA2_Channel1_IRQn); }

static int store_erase(void)
{
    int ok = 1;
    audio_pause();
    flash_unlock();
    for (uint32_t i = 0; i < STORE_PAGES && ok; i++) ok = flash_erase_page(STORE_ADDR + i * PAGE_SIZE);
    flash_lock_and_flush();
    audio_resume();
    return ok;
}

static int store_write(uint32_t offset, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    int ok = 1;
    audio_pause();
    flash_unlock();
    while (n && ok) {
        uint32_t page = STORE_ADDR + (offset / PAGE_SIZE) * PAGE_SIZE;
        uint32_t at = offset % PAGE_SIZE, k = PAGE_SIZE - at;
        if (k > n) k = n;
        const uint8_t *old = (const uint8_t *)page;
        uint8_t *b = (uint8_t *)page_buf;
        for (uint32_t i = 0; i < PAGE_SIZE; i++) b[i] = old[i];
        for (uint32_t i = 0; i < k; i++) b[at + i] = s[i];
        ok = flash_erase_page(page) && flash_program_page(page, page_buf);
        offset += k; s += k; n -= k;
    }
    flash_lock_and_flush();
    audio_resume();
    return ok;
}

/* --------------------------------------------------------------- microphone */
#define HALF_FRAMES 512                         /* I2S frames per DMA half (8.2 ms)  */
static int32_t dma_buf[2 * 2 * HALF_FRAMES];    /* left, right, left, right ...      */

/* Called for each half of the DMA buffer as it fills: average 4 samples
 * (62.5 kHz -> 15.625 kHz) and hand them to the recognizer, which computes
 * the band energies and finds words.  ~0.3 ms of CPU per 8 ms. */
static void audio_half(const int32_t *w)
{
    static int32_t x[HALF_FRAMES / 4];          /* static: keeps the 2 KB stack free */
    for (int i = 0; i < HALF_FRAMES / 4; i++) {
        int32_t s = 0;
        for (int k = 0; k < 4; k++) s += w[2 * (4 * i + k) + I2S_SLOT] >> 14;   /* 18-bit samples */
        x[i] = s >> 2;
    }
    rec_push(x, HALF_FRAMES / 4);
}

void DMA2_Channel1_IRQHandler(void)
{
    uint32_t isr = DMA2->ISR;
    if (isr & DMA_ISR_HTIF1) { DMA2->IFCR = DMA_IFCR_CHTIF1; audio_half(&dma_buf[0]); }
    if (isr & DMA_ISR_TCIF1) { DMA2->IFCR = DMA_IFCR_CTCIF1; audio_half(&dma_buf[2 * HALF_FRAMES]); }
}

/* ------------------------------------------------------------------ buttons */
/* PA0 and PA1 pull to ground when pressed (internal pull-ups).  Returns the
 * buttons that were just pressed, after 20 ms of debouncing. */
#define BTN_REC  1u                             /* PA0 */
#define BTN_REDO 2u                             /* PA1 */
static uint32_t btn_raw, btn_state, btn_t;

static uint32_t buttons_pressed(void)
{
    uint32_t raw = ~GPIOA->IDR & 3u;
    if (raw != btn_raw) { btn_raw = raw; btn_t = g_ms; return 0; }
    if (g_ms - btn_t < 20 || raw == btn_state) return 0;
    uint32_t down = raw & ~btn_state;
    btn_state = raw;
    return down;
}

/* ------------------------------------------------------- record / recognize */
#define NSTEPS (REC_NWORDS * REC_TPW)           /* 57 templates */
static int      recording, step;                /* step = which template is next */
static uint32_t confirm_until;                  /* PA0 pressed once: waiting for the 2nd */
static int      meter_peak;
static uint32_t t_meter;

static uint32_t tmpl_offset(int word, int take)
{
    return (uint32_t)(offsetof(rec_store_t, t) + (size_t)(word * REC_TPW + take) * sizeof(rec_tmpl_t));
}

static void upper(char *d, const char *s) { while (*s) { *d++ = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s; s++; } *d = 0; }

static void show_prompt(const char *note)
{
    char b[24], *p, name[12];
    int word = step % REC_NWORDS, take = step / REC_NWORDS;
    p = put_str(title, "REC "); p = put_int(p, (uint32_t)step + 1); p = put_str(p, "/"); put_int(p, NSTEPS);
    draw_clock(1);
    p = put_str(b, "SAY (TAKE "); p = put_int(p, (uint32_t)take + 1); p = put_str(p, "/"); p = put_int(p, REC_TPW);
    put_str(p, ")");
    disp_line(1, 1, b, 0);
    upper(name, rec_word_name[word]);
    disp_big(name);
    disp_line(5, 1, "", 0);
    disp_line(6, 1, note ? note : "PA1 = REDO LAST", 0);
}

static void show_listen(const char *note)
{
    put_str(title, "VIC");
    draw_clock(1);
    if (rec_store_valid(store)) {
        disp_line(1, 1, "SAY A WORD", 0);
        disp_big("READY");
    } else {
        disp_line(1, 1, "NOT RECORDED YET", 0);
        disp_big("PA0 X2");
    }
    disp_line(5, 1, "", 0);
    disp_line(6, 1, note ? note : "", 0);
}

static void start_recording(void)
{
    disp_line(6, 1, "ERASING...", 0);
    if (!store_erase()) { show_listen("FLASH ERASE FAILED"); log_str("flash erase failed\r\n"); return; }
    recording = 1;
    step = 0;
    log_str("recording 57 templates\r\n");
    show_prompt(0);
}

static void finish_recording(void)
{
    struct { uint32_t magic, thr_q8, tpw, reserved; } hdr;
    char b[32], *p;
    hdr.thr_q8 = rec_threshold(store, THRESH_K_Q4);
    hdr.magic = REC_STORE_MAGIC;
    hdr.tpw = REC_TPW;
    hdr.reserved = 0;
    recording = 0;
    if (!store_write(0, &hdr, sizeof hdr)) { show_listen("FLASH WRITE FAILED"); return; }
    p = put_str(b, "DONE. LIMIT "); p = put_db(p, hdr.thr_q8); put_str(p, " DB");
    show_listen(b);
    log_str(b); log_str("\r\n");
}

/* A word was spoken while recording: save it as the next template. */
static void record_word(rec_tmpl_t *w)
{
    char b[80], *p, name[12];
    int word = step % REC_NWORDS, take = step / REC_NWORDS;
    w->word = (uint8_t)word;
    if (!store_write(tmpl_offset(word, take), w, sizeof *w)) { show_prompt("FLASH WRITE FAILED"); return; }

    /* Quality check against what is recorded so far: the closest earlier take
     * of the same word, and the closest other word. */
    uint32_t own = REC_INF, other = REC_INF;
    int other_w = -1;
    for (int s = 0; s < step; s++) {
        const rec_tmpl_t *t = &store->t[s % REC_NWORDS][s / REC_NWORDS];
        uint32_t d = rec_dist(w, t);
        if (s % REC_NWORDS == word) { if (d < own) own = d; }
        else if (d < other) { other = d; other_w = s % REC_NWORDS; }
    }
    p = put_str(b, "rec "); p = put_str(p, rec_word_name[word]); p = put_str(p, " take ");
    p = put_int(p, (uint32_t)take + 1); p = put_str(p, " len "); p = put_int(p, (uint32_t)w->len * 16);
    p = put_str(p, "ms own "); p = put_db(p, own); p = put_str(p, " other "); p = put_db(p, other);
    put_str(p, "\r\n");
    log_str(b);

    step++;
    if (step == NSTEPS) { finish_recording(); return; }
    if (take > 0 && other < own && other_w >= 0) {      /* closer to another word: suggest a redo */
        upper(name, rec_word_name[other_w]);
        p = put_str(b, "SOUNDED LIKE "); put_str(p, name);       /* PA1 redoes it */
        show_prompt(b);
    } else {
        p = put_str(b, "OK "); p = put_int(p, (uint32_t)w->len * 16); put_str(p, " MS. PA1 = REDO");
        show_prompt(b);
    }
}

/* A word was spoken while listening: match it with DTW. */
static void recognize_word(const rec_tmpl_t *w)
{
    char b[80], *p, name[12];
    rec_result_t r;
    if (!rec_store_valid(store)) { disp_line(6, 1, "RECORD FIRST: PA0 X2", 0); return; }
    rec_classify(w, store, &r);
    if (r.word >= 0) { upper(name, rec_word_name[r.word]); disp_big(name); }
    else disp_big("??");
    p = put_str(b, "DIST "); p = put_db(p, r.dist); p = put_str(p, " LIMIT "); put_db(p, store->thr_q8);
    disp_line(5, 1, b, 0);
    if (r.second >= 0) {
        upper(name, rec_word_name[r.word >= 0 ? r.second : r.best]);
        p = put_str(b, r.word >= 0 ? "NEXT " : "CLOSEST "); p = put_str(p, name); *p++ = ' ';
        put_db(p, r.word >= 0 ? r.dist2 : r.dist);
        disp_line(6, 1, b, 0);
    }
    p = put_str(b, r.word >= 0 ? rec_word_name[r.word] : "??");
    p = put_str(p, "  best "); p = put_str(p, r.best >= 0 ? rec_word_name[r.best] : "-"); *p++ = ' ';
    p = put_db(p, r.dist);
    p = put_str(p, "  next "); p = put_str(p, r.second >= 0 ? rec_word_name[r.second] : "-"); *p++ = ' ';
    p = put_db(p, r.dist2);
    put_str(p, "\r\n");
    log_str(b);
}

static void on_buttons(uint32_t down)
{
    if (down & BTN_REC) {
        if (g_ms < confirm_until) { confirm_until = 0; start_recording(); }
        else {
            confirm_until = g_ms + 3000;
            disp_line(6, 1, "PA0 AGAIN = RECORD ALL", 0);
        }
    }
    if ((down & BTN_REDO) && recording && step > 0) {
        step--;
        show_prompt("REDO THIS ONE");
    }
}

static void loop_once(void)
{
    static rec_tmpl_t w;                                 /* 608 bytes: not on the stack */
    uint32_t down = buttons_pressed();
    if (down) on_buttons(down);
    if (confirm_until && g_ms >= confirm_until) {        /* second PA0 press never came */
        confirm_until = 0;
        disp_line(6, 1, "", 0);
    }
    if (rec_poll(&w)) {
        if (recording) record_word(&w);
        else recognize_word(&w);
    }
    int lvl = rec_stats.level - rec_stats.floor;
    if (lvl > meter_peak) meter_peak = lvl;
    if (g_ms - t_meter >= 200) {                         /* clock and level meter, 5 times a second */
        char b[24], *p = put_str(b, "MIC ");
        int bars = meter_peak / 6;
        if (bars > 16) bars = 16;
        while (bars-- > 0) *p++ = '#';
        *p = 0;
        draw_clock(0);
        disp_line(7, 1, b, 0);
        meter_peak = 0;
        t_meter = g_ms;
    }
}

/* ----------------------------------------------------------------- hardware */
static void hw_init(void)
{
    /* 80 MHz from the internal 4 MHz oscillator (x40 / 2) */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS;
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_MSI | (40u << RCC_PLLCFGR_PLLN_Pos) | RCC_PLLCFGR_PLLREN;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) { }
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) { }

    RCC->AHB2ENR  |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART2EN | RCC_APB1ENR1_SPI3EN;

    SysTick->LOAD = 80000u - 1u;                                /* 1 ms tick */
    SysTick->VAL  = 0;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

    /* PA0, PA1 = buttons: inputs with pull-ups */
    GPIOA->MODER &= ~(0xFu << 0);
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~(0xFu << 0)) | (0x5u << 0);

    /* PA2 = USART2_TX (AF7), 115200 baud */
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFu << 8)) | (7u << 8);
    GPIOA->MODER  = (GPIOA->MODER & ~(3u << 4)) | (2u << 4);
    USART2->BRR = 80000000u / 115200u;
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;

    /* SPI3 to the FPGA: PB3 = SCK, PB5 = MOSI (AF6), PB4 = chip select; mode 0, 312.5 kHz */
    GPIOB->BSRR   = 1u << 4;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFu << 12) | (0xFu << 20))) | (6u << 12) | (6u << 20);
    GPIOB->MODER  = (GPIOB->MODER & ~((3u << 6) | (3u << 8) | (3u << 10))) | (2u << 6) | (1u << 8) | (2u << 10);
    SPI3->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
    SPI3->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (7u << SPI_CR1_BR_Pos) | SPI_CR1_SPE;
}

static void mic_start(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    RCC->APB2ENR |= RCC_APB2ENR_SAI1EN;
    /* SAI clock = exactly 4 MHz from the second PLL (PLLSAI1): 4 MHz x 24 / 24.
     * The SAI then uses it undivided as the bit clock, so 4 MHz / 64 = 62.5 kHz. */
    RCC->CR &= ~RCC_CR_PLLSAI1ON;
    while (RCC->CR & RCC_CR_PLLSAI1RDY) { }
    RCC->PLLSAI1CFGR = (24u << 8) | (24u << 27) | RCC_PLLSAI1CFGR_PLLSAI1PEN;
    RCC->CR |= RCC_CR_PLLSAI1ON;
    while (!(RCC->CR & RCC_CR_PLLSAI1RDY)) { }
    RCC->CCIPR &= ~RCC_CCIPR_SAI1SEL;                           /* 00 = PLLSAI1 "P" output */

    /* PA8 = bit clock, PA9 = word select, PA10 = data in (all AF13) */
    GPIOA->AFR[1] = (GPIOA->AFR[1] & ~0xFFFu) | 0xDDDu;
    GPIOA->MODER  = (GPIOA->MODER & ~(0x3Fu << 16)) | (0x2Au << 16);

    /* DMA2 channel 1 <- SAI1 block A: 32-bit words into a circular buffer */
    DMA2_CSELR->CSELR = (DMA2_CSELR->CSELR & ~DMA_CSELR_C1S) | (1u << DMA_CSELR_C1S_Pos);
    DMA2_Channel1->CPAR  = (uint32_t)&SAI1_Block_A->DR;
    DMA2_Channel1->CMAR  = (uint32_t)dma_buf;
    DMA2_Channel1->CNDTR = 2 * 2 * HALF_FRAMES;
    DMA2_Channel1->CCR   = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_HTIE | DMA_CCR_TCIE |
                           DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_EN;
    NVIC_SetPriority(DMA2_Channel1_IRQn, 1);
    NVIC_EnableIRQ(DMA2_Channel1_IRQn);

    /* I2S master receiver: 64 bit clocks per sample, 4 MHz / 64 = 62.5 kHz */
    SAI1_Block_A->FRCR  = (64u - 1u) | ((32u - 1u) << 8) | SAI_xFRCR_FSDEF | SAI_xFRCR_FSOFF;
    SAI1_Block_A->SLOTR = (1u << 8) | (3u << 16);
    SAI1_Block_A->CR2   = 0;
    SAI1_Block_A->CR1   = SAI_xCR1_MODE_0 | SAI_xCR1_DS | SAI_xCR1_CKSTR | SAI_xCR1_NODIV |
                          SAI_xCR1_DMAEN;
    SAI1_Block_A->CR1  |= SAI_xCR1_SAIEN;
}

int main(void)
{
    hw_init();
    log_str("\r\nVIC recognizer\r\n");
#if STEP == 1
    uint32_t last = 0, secs = 0;
    disp_clear();
    disp_line(0, 1, "STEP 1: MCU TO FPGA", 0);
    disp_line(2, 3, "HELLO", 1);
    for (;;) {
        if (g_ms - last >= 1000) {
            char b[24], *p = put_str(b, "SECONDS ");
            last += 1000;
            put_int(p, ++secs);
            disp_line(7, 1, b, 0);
            log_str(".");
        }
    }
#else
    rtc_init();                                 /* can take a few seconds the first time */
    rec_init();
    disp_clear();
    show_listen(0);
    log_str(rec_store_valid(store) ? "templates loaded from flash\r\n" : "no templates yet: press PA0 twice\r\n");
    mic_start();
    for (;;) loop_once();
#endif
}
