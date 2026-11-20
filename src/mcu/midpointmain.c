/* Reading the DEBUG rows (refreshed five times a second):
 *
 *   row 1   L xxxxx  R xxxxx     one raw 18-bit sample from the left and right channel, in hex
 *   row 5   IRQ n  E n  F n      buffers received (counts up, wraps at 1000) / loudness now / noise floor
 *
 *   IRQ not counting          the SAI or its clock is not running: no samples are arriving at all
 *   IRQ counts, L = 00000     the mic's data never reaches PA10 (wiring, mic power, or SW7 RX closed)
 *   L = 00000, R changes      mic is on the other channel: set I2S_SLOT to 1 (or tie SEL to GND)
 *   L = 3FFFF or stuck        data line floating or shorted
 *   L changes, E stays near F even when you speak    mic too far away, or its sound hole is blocked
 *   E jumps 27+ above F when you speak               detection should work; words need a 130 ms pause after them
 *
 * Teaching it your voice (enroll.c): hold the button on PA0 (A0 on the Nucleo, to GND) for one second.
 * The screen asks for every word three times; say each when prompted. If a recording does not sound like
 * the earlier ones it asks again. When the last word is done the screen says DONE and the device starts
 * recognising: the middle of the screen shows the word it heard and, underneath, how close the match was
 * ("MATCH 1.2 OF 1.7": distance of this word and the limit, in dB). NOT A WORD means it was too far from
 * everything taught. Hold PA0 again to teach again, or to cancel while teaching.
 * When teaching finishes, the recordings are saved in flash (last 32 KB) and loaded again at power-up,
 * so teaching is needed only once. Teaching again replaces them; cancelling keeps the old ones.
 *
 * Calculator (calc.c): once taught, recognised words drive a pocket calculator. Say one word at a
 * time, e.g. "seven multiply six equals". The big number is the calculator's display, the line above
 * it the sum so far ("7 X 6 ="), the line below it the word just heard and its match distance.
 *
 * Clock and timer (modes.c, clock.c, timer.c): say "clock" to see the time, "clock" again to go back
 * to the calculator; same for "timer". Nothing is lost by switching: a half-typed sum waits, and the
 * timer keeps counting (shown top right as TMR 01:23).
 *   Clock: say the time hours first; it is set at the 4th digit: "one zero three zero" = 10:30,
 *   "nine three zero" = 09:30. Impossible digits are refused as they are heard (see clock.h). Until
 *   it is set by voice after power-up the clock runs from the build time and says CLOCK NOT SET.
 *   Timer: digits then "start", e.g. "one three zero start" = 1 min 30 s; "stop" pauses, "start"
 *   carries on, "clear" resets. At zero the screen jumps to TIME UP (blinking); any word stops it.
 *
 * E and F are now measured on the filtered 15.6 kHz signal, so their absolute numbers differ from the
 * checkoff build; only the gap between them matters. The detector margins live in wordcap.h.
 */

#include <stdint.h>
#include "stm32l432xx.h"
#include "frontend.h"       /* mic samples -> 12-band feature frame every 16 ms */
#include "wordcap.h"        /* finds a word in the frames and keeps them        */
#include "splash.h"         /* start-up animation                               */
#include "enroll.h"         /* teach the device your voice, recognise words     */
#include "calc.h"           /* recognised words -> calculator                   */
#include "modes.h"          /* calculator / clock / timer, switched by voice    */
#include "timer.h"
#include "clock.h"

#define STEP 2          /* 1 = no microphone: just prove MCU -> FPGA -> OLED (shows HELLO)
                           2 = the checkoff demo                                          */

#define I2S_SLOT    0   /* 0 = mic SEL tied to GND, 1 = SEL tied to 3V3                     */
#define DEBUG       0   /* 1 = show raw microphone numbers on rows 1 and 5 (see top of file).
                           Off by default: row 1 shows the calculator's sum                     */
#define DUMP_WORDS  0   /* 1 = after each word, print its feature frames on the serial port,
                           one line per 16 ms frame, 12 bands as 24 hex digits (for comparing
                           with dtw_poc.py)                                                  */
#define BYPASS_BUTTON 1 /* 1 = PA1 (button to GND) turns the anti-alias filter off while held   */
#define STARTUP_ANIM  1 /* 1 = play the 2-second start-up animation (splash.c)                  */

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
static void disp_blink(uint8_t pages) { uint8_t b[2] = { 0x03, pages }; fpga_send(b, 2); }  /* bit n = page n */

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

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_int(char *p, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0; return p;
}

#if DEBUG
static char *put_hex5(char *p, uint32_t v)
{
    for (int s = 16; s >= 0; s -= 4) *p++ = "0123456789ABCDEF"[(v >> s) & 15];
    *p = 0; return p;
}
#endif

static char *put_2d(char *p, unsigned v)             /* two digits, e.g. 07 */
{
    *p++ = (char)('0' + (v / 10) % 10); *p++ = (char)('0' + v % 10); *p = 0; return p;
}

/* ---------------------------------------------------------- real-time clock */
/* The RTC is a separate counter inside the chip, driven by the 32.768 kHz
 * watch crystal on the Nucleo (LSE).  It keeps hours, minutes and seconds by
 * itself; the program only sets it once and then reads it.  It keeps running
 * through a reset, and restarts from the build time after power is removed. */
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

/* Has the time been set by voice? Kept in RTC backup register 0, which lives with the RTC:
 * it survives a reset, and is wiped together with the time when the power goes. */
#define CLOCK_SET_MARK 0xC10C5E7Du
static int clock_set;

static void rtc_init(void)
{
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN | RCC_APB1ENR1_RTCAPBEN;
    PWR->CR1 |= PWR_CR1_DBP;                            /* allow writes to the RTC domain */
    if ((RCC->BDCR & RCC_BDCR_RTCEN) && (RTC->ISR & RTC_ISR_INITS)) {          /* already running */
        clock_set = RTC->BKP0R == CLOCK_SET_MARK;
        return;
    }
    clock_set = 0;                                      /* fresh start: the time below is only the build time */

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

static unsigned shown_sec = 99;
/* Top row: the mode on the left; on the right HH:MM:SS, or the timer ("TMR 01:23") while it is
 * running or paused and the timer screen is not showing. Set shown_sec = 99 to force a redraw. */
static void draw_clock(void)
{
    unsigned h, m, sec;
    char b[24], *p;
    rtc_get(&h, &m, &sec);
    if (sec == shown_sec) return;
    shown_sec = sec;
    int md = mode_get();
    p = put_str(b, md == MODE_CLOCK ? "CLOCK" : md == MODE_TIMER ? "TIMER" : "CALCULATOR");
    while (p < b + 12) *p++ = ' ';
    if (tmr_active() && md != MODE_TIMER) { p = put_str(p, "TMR "); tmr_text(p, g_ms); }
    else { p = put_2d(p, h); *p++ = ':'; p = put_2d(p, m); *p++ = ':'; put_2d(p, sec); }
    disp_line(0, 1, b, 0);
}

/* --------------------------------------------------------------- microphone */
#define HALF_FRAMES 512                         /* I2S frames per DMA half (8.2 ms)  */
static int32_t dma_buf[2 * 2 * HALF_FRAMES];    /* left, right, left, right ...      */

static volatile uint32_t dbg_irq, dbg_l, dbg_r, dbg_e, dbg_isr_us;     /* for the DEBUG rows */

/* feature frames, handed from the DMA interrupt to the main loop */
#define FQ 32                                   /* must be a power of two (32 frames = 0.5 s) */
static fe_frame_t fq[FQ];
static volatile uint8_t fq_head, fq_tail;

/* Called for each half of the DMA buffer as it fills (every 8.2 ms). */
static void audio_half(const int32_t *w)
{
#if DEBUG
    uint32_t t0 = DWT->CYCCNT;
#endif
    dbg_irq++;
    dbg_l = ((uint32_t)w[0] >> 14) & 0x3FFFF;   /* one raw sample from each channel */
    dbg_r = ((uint32_t)w[1] >> 14) & 0x3FFFF;
    for (int i = 0; i < HALF_FRAMES; i++) {
        fe_frame_t fr;
        /* the mic's 18-bit sample sits in the top of the 32-bit word: >> 16 leaves 16 bits */
        if (fe_push((int16_t)(w[2 * i + I2S_SLOT] >> 16), &fr)) {
            dbg_e = fr.loud;
            uint8_t next = (uint8_t)((fq_head + 1) & (FQ - 1));
            if (next != fq_tail) {
                fq[fq_head] = fr;
                __asm volatile("" ::: "memory");        /* frame must be written before the index moves */
                fq_head = next;
            }
        }
    }
#if DEBUG
    uint32_t us = (DWT->CYCCNT - t0) / 80;      /* 80 MHz core clock */
    if (us > dbg_isr_us) dbg_isr_us = us;
#endif
}

void DMA2_Channel1_IRQHandler(void)
{
    uint32_t isr = DMA2->ISR;
    if (isr & DMA_ISR_HTIF1) { DMA2->IFCR = DMA_IFCR_CHTIF1; audio_half(&dma_buf[0]); }
    if (isr & DMA_ISR_TCIF1) { DMA2->IFCR = DMA_IFCR_CTCIF1; audio_half(&dma_buf[2 * HALF_FRAMES]); }
}

/* ---------------------------------------------------------- saved teaching */
/* What teaching learned (enroll.c) is kept in the last 32 KB of flash, pages 112..127, so it
 * survives power-off. The program itself must stay below 0x08038000 (224 KB); it is about 20 KB. */
#define SAVE_ADDR   0x08038000u
#define SAVE_PAGES  16u
#define PAGE_SIZE   2048u
typedef char save_fits_in_flash[(ENR_SAVE_MAX <= SAVE_PAGES * PAGE_SIZE) ? 1 : -1];
#define FLASH_ERRORS (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | \
                      FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR)

static uint32_t fl_addr, fl_fill;               /* where the next 8 bytes go / bytes collected */
static uint64_t fl_dw;                          /* flash is written 8 bytes (a "doubleword") at a time */
static int      fl_ok;

static void flash_wait(void) { while (FLASH->SR & FLASH_SR_BSY) { } }

static void fl_write_dw(void)
{
    if (fl_addr + 8 > SAVE_ADDR + SAVE_PAGES * PAGE_SIZE) { fl_ok = 0; return; }
    FLASH->CR |= FLASH_CR_PG;
    *(volatile uint32_t *)fl_addr       = (uint32_t)fl_dw;
    *(volatile uint32_t *)(fl_addr + 4) = (uint32_t)(fl_dw >> 32);
    flash_wait();
    if (FLASH->SR & FLASH_ERRORS) fl_ok = 0;
    FLASH->SR = FLASH_SR_EOP | FLASH_ERRORS;
    FLASH->CR &= ~FLASH_CR_PG;
    fl_addr += 8; fl_dw = ~0ull; fl_fill = 0;
}

static void fl_put(const void *src, uint32_t n)        /* enr_save() hands its bytes to this */
{
    const uint8_t *s = src;
    while (n--) {
        fl_dw = (fl_dw & ~(0xFFull << (8 * fl_fill))) | ((uint64_t)*s++ << (8 * fl_fill));
        if (++fl_fill == 8) fl_write_dw();
    }
}

/* Erase the save area, write what teaching learned, read it back. About 0.6 s; audio is
 * paused meanwhile (the CPU stalls while flash is busy anyway). Returns 1 if it worked. */
static int save_teaching(void)
{
    NVIC_DisableIRQ(DMA2_Channel1_IRQn);
    flash_wait();
    if (FLASH->CR & FLASH_CR_LOCK) { FLASH->KEYR = 0x45670123u; FLASH->KEYR = 0xCDEF89ABu; }
    FLASH->SR = FLASH_SR_EOP | FLASH_ERRORS;
    fl_ok = 1;
    for (uint32_t i = 0; i < SAVE_PAGES && fl_ok; i++) {
        uint32_t page = (SAVE_ADDR - FLASH_BASE) / PAGE_SIZE + i;
        FLASH->CR = (FLASH->CR & ~(FLASH_CR_PNB | FLASH_CR_PG)) | FLASH_CR_PER | (page << FLASH_CR_PNB_Pos);
        FLASH->CR |= FLASH_CR_STRT;
        flash_wait();
        FLASH->CR &= ~FLASH_CR_PER;
        if (FLASH->SR & FLASH_ERRORS) fl_ok = 0;
    }
    fl_addr = SAVE_ADDR; fl_dw = ~0ull; fl_fill = 0;
    if (fl_ok && !enr_save(fl_put)) fl_ok = 0;
    if (fl_ok && fl_fill) fl_write_dw();                /* last few bytes (the rest stays FF) */
    FLASH->CR |= FLASH_CR_LOCK;
    FLASH->ACR &= ~FLASH_ACR_DCEN;                      /* forget cached copies of the old contents */
    FLASH->ACR |= FLASH_ACR_DCRST;
    FLASH->ACR &= ~FLASH_ACR_DCRST;
    FLASH->ACR |= FLASH_ACR_DCEN;
    if (fl_ok) fl_ok = enr_load((const void *)SAVE_ADDR);          /* check it reads back */
    fe_init(); wc_init(); fq_tail = fq_head;            /* audio skipped: start the detector fresh */
    NVIC_EnableIRQ(DMA2_Channel1_IRQn);
    return fl_ok;
}

/* ------------------------------------------------------------ word detector */
static uint32_t words, last_ms, t_meter, t_done;    /* t_done: when the "teaching complete" message ends */
static char     big[12] = "READY";
static int      bypass_now, big_scale;

/* The big word in the middle of the screen: 3x size when it fits (7 letters), else 2x. */
static void show_big(const char *w)
{
    int len = 0, sc;
    while (w[len]) len++;
    sc = len <= 7 ? 3 : 2;
    if (sc != big_scale) {                              /* the two sizes cover different rows: wipe the old one */
        for (int pg = 2; pg <= 4; pg++) disp_line(pg, 1, "", 0);
        big_scale = sc;
    }
    disp_line(2, sc, w, 1);
}

static void draw(const char *note)
{
    char b[24], *p;
    show_big(big);
    if (words) { p = put_str(b, "LENGTH "); p = put_int(p, last_ms); put_str(p, " MS"); }
    else if (enr_state() == ENR_IDLE) put_str(b, "HOLD A0 TO TEACH");
    else b[0] = 0;
    disp_line(6, 1, note ? note : b, 0);
}

/* The calculator screen: the sum so far on row 1, the display big on rows 2..4, a note on row 6. */
static void show_calc(const char *note)
{
    disp_line(1, 1, calc_expr(), 0);
    put_str(big, calc_display());
    show_big(big);
    disp_line(6, 1, note, 0);
}

/* Clock and timer: the big line is the time, redrawn only when the text changes. */
static char shown_big[12];
static void update_big(void)
{
    char t[12], *p;
    if (mode_get() == MODE_CLOCK && clk_typing()) {
        clk_entry(t);                                                               /* 10:3_ while typing */
    } else if (mode_get() == MODE_CLOCK) {
        unsigned h, m, s;
        rtc_get(&h, &m, &s);
        p = put_2d(t, h); *p++ = ':'; p = put_2d(p, m); *p++ = ':'; put_2d(p, s);     /* 14:32:05 */
    } else if (mode_get() == MODE_TIMER && tmr_state() != TMR_ALARM) {
        tmr_text(t, g_ms);                                                          /* 01:30 */
    } else return;
    int same = 1;
    for (int i = 0; t[i] || shown_big[i]; i++) if (t[i] != shown_big[i]) { same = 0; break; }
    if (same) return;
    put_str(shown_big, t);
    show_big(t);
}

/* The whole screen for the current mode (once taught). note = the line under the big text. */
static void draw_mode(const char *note)
{
    if (!note) note = "";
    shown_sec = 99; draw_clock();                       /* the title follows the mode */
    if (tmr_state() == TMR_ALARM) {                     /* the timer went off: blink until any word */
        disp_line(1, 1, "TIMER DONE", 0);
        show_big("TIME UP");
        disp_line(6, 1, *note ? note : "SAY ANY WORD", 0);
        disp_blink(0x1C);                               /* pages 2..4: the big text */
        return;
    }
    disp_blink(0);
    shown_big[0] = 0;                                   /* make update_big redraw */
    switch (mode_get()) {
    case MODE_CLOCK:
        disp_line(1, 1, clk_typing() ? "SETTING THE CLOCK"
                      : clock_set   ? "SAY A TIME TO CHANGE"
                      :               "CLOCK NOT SET: SAY IT", 0);
        update_big();
        break;
    case MODE_TIMER:
        disp_line(1, 1, tmr_state() == TMR_RUNNING ? "RUNNING. STOP=PAUSE"
                      : tmr_state() == TMR_PAUSED  ? "PAUSED. START/CLEAR"
                      :                              "DIGITS, THEN START", 0);
        update_big();
        break;
    default:
        show_calc(*note ? note : "SAY A SUM");
        return;
    }
    disp_line(6, 1, note, 0);
}

/* distance in 1/16 count -> "1.2" (dB; one dB is 2.55 counts) */
static char *put_db(char *p, int32_t units)
{
    int32_t tenths = (units * 100 + 204) / 408;          /* tenths of a dB: units / 40.8 * 10 */
    p = put_int(p, (uint32_t)(tenths / 10)); *p++ = '.'; *p++ = (char)('0' + tenths % 10); *p = 0;
    return p;
}

#if DUMP_WORDS
static void dump_word(void)
{
    static const char hx[] = "0123456789ABCDEF";
    char b[2 * FE_BANDS + 3];
    for (int i = 0; i < wc_word.n; i++) {
        char *p = b;
        for (int k = 0; k < FE_BANDS; k++) { *p++ = hx[wc_word.f[i][k] >> 4]; *p++ = hx[wc_word.f[i][k] & 15]; }
        *p++ = '\r'; *p++ = '\n'; *p = 0;
        log_str(b);
    }
}
#endif

/* ------------------------------------------------------------------ teaching */
static void teach_draw(const char *status)
{
    char b[24], *p;
    p = put_str(b, "TEACH WORD "); p = put_int(p, (uint32_t)enr_word() + 1);
    p = put_str(p, " OF "); put_int(p, (uint32_t)enr_word_count());
    disp_line(0, 1, b, 0);
    p = put_str(b, "SAY IT "); p = put_int(p, (uint32_t)enr_sample() + 1);
    p = put_str(p, " OF "); put_int(p, ENR_SAMPLES);
    disp_line(1, 1, b, 0);
    show_big(enr_name(enr_word()));
    p = put_int(b, (uint32_t)enr_done()); p = put_str(p, " OF "); p = put_int(p, (uint32_t)enr_total());
    put_str(p, " DONE");
    disp_line(5, 1, b, 0);
    disp_line(6, 1, status, 0);
}

static void screen_init(void);

static void teach_word(void)                    /* a complete word was heard while teaching */
{
    char b[96], *p;
    int word = enr_word(), sample = enr_sample();
    int32_t dist;
    int r = enr_submit((const uint8_t (*)[FE_BANDS])wc_word.f, wc_word.n, &dist);

    p = put_str(b, "teach "); p = put_str(p, enr_name(word)); p = put_str(p, " ");
    p = put_int(p, (uint32_t)sample + 1); p = put_str(p, "/"); p = put_int(p, ENR_SAMPLES);
    p = put_str(p, "  frames="); p = put_int(p, wc_word.n);
    if (dist >= 0) { p = put_str(p, "  d="); p = put_db(p, dist); }
    switch (r) {
    case ENR_GOT_IT:        p = put_str(p, "  ok");                teach_draw("GOT IT - SAY IT AGAIN"); break;
    case ENR_WORD_DONE:     p = put_str(p, "  ok, next word");     teach_draw("GOT IT - NEXT WORD");    break;
    case ENR_TOO_DIFFERENT: p = put_str(p, "  too different");     teach_draw("TOO DIFFERENT - AGAIN"); break;
    case ENR_RESTARTED:     p = put_str(p, "  word restarted");    teach_draw("RESTARTING THIS WORD");  break;
    case ENR_NO_MEMORY:
        p = put_str(p, "  OUT OF MEMORY");
        enr_abort(); screen_init(); disp_line(6, 1, "OUT OF MEMORY - RETRY", 0);
        break;
    case ENR_FINISHED: {
        p = put_str(p, "  all done");
        disp_line(0, 1, "TEACHING COMPLETE", 0);
        disp_line(1, 1, "", 0);
        show_big("SAVING");
        disp_line(5, 1, "", 0);
        disp_line(6, 1, "", 0);
        int saved = save_teaching();            /* keep it through power-off */
        p = put_str(p, saved ? ", saved to flash" : ", FLASH SAVE FAILED");
        show_big("DONE");
        disp_line(6, 1, saved ? "SAVED. SAY A SUM" : "NOT SAVED - FLASH ERR", 0);
        mode_reset();
        t_done = g_ms + 2000u;                  /* then the calculator screen comes back */
        break;
    }
    default: break;
    }
    put_str(p, "\r\n"); log_str(b);
}

/* ---------------------------------------------------------------- recognising */
static void recognise_word(void)                /* a complete word was heard after teaching */
{
    char b[96], *p;
    int best;
    int32_t dist, lim;
    int w = enr_recognize((const uint8_t (*)[FE_BANDS])wc_word.f, wc_word.n, &best, &dist, &lim);

    words++;
    last_ms = (uint32_t)wc_word_ms();
    if (w < 0) {                                        /* too far from every taught word */
        p = put_str(b, "SAY AGAIN ("); p = put_str(p, enr_name(best)); put_str(p, "?)");
    } else {
        int h = 0, m = 0, r = mode_word(w, g_ms, &h, &m);
        switch (r) {
        case MW_SWITCHED:
            put_str(b, mode_get() == MODE_CLOCK ? (clock_set ? "SAY CLOCK TO GO BACK" : "SAY 4 DIGITS: HHMM")
                     : mode_get() == MODE_TIMER ? "SAY TIMER TO GO BACK" : "BACK TO CALCULATOR");
            break;
        case MW_ALARM_OFF:    put_str(b, "ALARM OFF"); break;
        case MW_CLOCK_SET:
            rtc_set((unsigned)h, (unsigned)m, 0);
            RTC->BKP0R = CLOCK_SET_MARK;                /* the time is real now */
            clock_set = 1;
            p = put_str(b, "CLOCK SET TO "); p = put_2d(p, (unsigned)h); *p++ = ':'; put_2d(p, (unsigned)m);
            break;
        case MW_CLOCK_BAD_HOUR: put_str(b, "HOURS ONLY GO TO 23"); break;
        case MW_CLOCK_BAD_MIN:  put_str(b, "MINUTES ONLY GO TO 59"); break;
        case MW_CLOCK_CANCEL: put_str(b, "CANCELLED"); break;
        case MW_STOP_FIRST:   put_str(b, "SAY STOP FIRST"); break;
        case MW_NOT_HERE:     p = put_str(b, enr_name(w)); put_str(p, " - NOT HERE"); break;
        default:                                        /* used: "SEVEN 1.2 OF 1.7" */
            p = put_str(b, enr_name(w)); p = put_str(p, " "); p = put_db(p, dist); p = put_str(p, " OF "); put_db(p, lim);
            break;
        }
    }
    draw_mode(b);

    p = put_str(b, w >= 0 ? "HEARD " : "UNKNOWN nearest=");
    p = put_str(p, w >= 0 ? enr_name(w) : enr_name(best));
    p = put_str(p, "  d="); p = put_db(p, dist); p = put_str(p, " limit="); p = put_db(p, lim);
    p = put_str(p, "  frames="); p = put_int(p, wc_word.n);
#if DEBUG
    p = put_str(p, "  isr_us="); p = put_int(p, dbg_isr_us);
#endif
    put_str(p, "\r\n"); log_str(b);
}

static void on_frame(const fe_frame_t *fr)      /* one 16 ms feature frame */
{
    int r = wc_feed(fr);
    if (r == WC_NONE) return;
    if (r == WC_SHORT) { log_str("click ignored\r\n"); return; }
    if (r == WC_LONG) {
        if (enr_state() == ENR_TEACHING) teach_draw("TOO LONG - AGAIN");
        else if (enr_state() == ENR_READY) draw_mode("TOO LONG - TRY AGAIN");
        else draw("TOO LONG - TRY AGAIN");
        log_str("sound too long\r\n");
        return;
    }
    if (enr_state() == ENR_TEACHING) { teach_word(); return; }
    if (enr_state() == ENR_READY)    { recognise_word(); return; }

    char b[64], *p;                             /* nothing taught yet: just report the word */
    words++;
    last_ms = (uint32_t)wc_word_ms();
    p = put_str(big, "WORD "); put_int(p, words);
    draw(0);
    p = put_str(b, "WORD "); p = put_int(p, words); p = put_str(p, "  ms="); p = put_int(p, last_ms);
    p = put_str(p, "  frames="); p = put_int(p, wc_word.n);
#if DEBUG
    p = put_str(p, "  isr_us="); p = put_int(p, dbg_isr_us);
#endif
    put_str(p, "\r\n"); log_str(b);
#if DUMP_WORDS
    dump_word();
#endif
}

/* splash.c draws through these two */
void     splash_send(const uint8_t *buf, int n) { fpga_send(buf, n); }
uint32_t splash_ms(void)                        { return g_ms; }

static void screen_init(void)
{
    disp_clear();
    big_scale = 0;
    shown_sec = 99;
    draw_clock();
    if (enr_state() == ENR_READY) { draw_mode(0); return; }   /* taught: calculator / clock / timer */
    disp_line(1, 1, "SAY A WORD", 0);
    draw(0);
}

/* hold PA0 (pressed = low) for one second: start teaching, or cancel it */
static void check_teach_button(void)
{
    static uint32_t since;
    static int down, used;
    if (GPIOA->IDR & 1u) { down = 0; return; }             /* not pressed */
    if (!down) { down = 1; used = 0; since = g_ms; return; }
    if (used || g_ms - since < 1000u) return;
    used = 1;
    t_done = 0;
    if (enr_state() == ENR_TEACHING) {
        enr_abort();
        words = 0; put_str(big, "READY");
        int back = enr_load((const void *)SAVE_ADDR);   /* go back to the last saved teaching, if any */
        mode_reset();
        screen_init();
        log_str(back ? "teaching cancelled, saved words loaded again\r\n" : "teaching cancelled\r\n");
    } else {
        enr_begin();
        wc_init();
        words = 0; put_str(big, "READY");
        disp_clear();
        big_scale = 0;
        teach_draw("SAY THE WORD");
        log_str("teaching started\r\n");
    }
}

static void loop_once(void)
{
    while (fq_tail != fq_head) {
        fe_frame_t fr = fq[fq_tail];
        __asm volatile("" ::: "memory");
        fq_tail = (uint8_t)((fq_tail + 1) & (FQ - 1));
        on_frame(&fr);
    }
    check_teach_button();
    if (t_done && (int32_t)(g_ms - t_done) >= 0) { t_done = 0; screen_init(); }
    if (g_ms - t_meter >= 200) {                /* clock and level meter, 5 times a second */
        int busy = enr_state() == ENR_TEACHING || t_done;     /* the teaching screens use rows 0 and 1 */
        if (enr_state() == ENR_READY) {
            if (mode_tick(g_ms)) {                              /* the timer reached zero */
                log_str("timer done\r\n");
                if (!busy) draw_mode(0);
            } else if (!busy) update_big();                     /* clock / timer digits */
        }
        if (!busy) draw_clock();
        char b[24], *p = put_str(b, "MIC ");
        int bars = wc_take_peak() / 6;
        if (bars > 16) bars = 16;
        while (bars-- > 0) *p++ = '#';
        *p = 0;
        disp_line(7, 1, b, 0);
#if DEBUG
        if (!busy) {
            p = put_str(b, "L "); p = put_hex5(p, dbg_l); p = put_str(p, "  R "); put_hex5(p, dbg_r);
            disp_line(1, 1, b, 0);
            p = put_str(b, "IRQ "); p = put_int(p, dbg_irq % 1000); p = put_str(p, " E "); p = put_int(p, dbg_e);
            p = put_str(p, " F "); put_int(p, (uint32_t)wc_floor());
            disp_line(5, 1, b, 0);
        }
#endif
#if BYPASS_BUTTON
        int byp = !(GPIOA->IDR & (1u << 1));                    /* pressed = low */
        if (byp != bypass_now) {
            bypass_now = byp;
            fe_set_bypass(byp);
            log_str(byp ? "filter OFF\r\n" : "filter ON\r\n");
        }
#endif
        t_meter = g_ms;
    }
}

/* ----------------------------------------------------------------- hardware */
static void hw_init(void)
{
    /* 80 MHz from the internal 4 MHz oscillator (x40 / 2) */
    /* 4 wait states at 80 MHz; prefetch and the instruction/data caches keep the audio interrupt fast */
    FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_4WS |
                 FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
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

    /* PA0 = "teach" button, pressed = low (input with pull-up); hold it for one second */
    GPIOA->MODER &= ~(3u << 0);
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~(3u << 0)) | (1u << 0);

    /* PA1 = "filter bypass" button, pressed = low (input with pull-up) */
    GPIOA->MODER &= ~(3u << 2);
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~(3u << 2)) | (1u << 2);

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
#if DEBUG
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;             /* cycle counter, to time the interrupt */
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
#endif
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
    log_str("\r\nmidpoint (single file)\r\n");
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
    { uint32_t t0 = g_ms; while (g_ms - t0 < 1500u) { } }   /* let the FPGA finish booting before the first packets */
#if STARTUP_ANIM
    splash_run();
#endif
    fe_init();
    wc_init();
    enr_abort();                                /* nothing taught yet...              */
    int loaded = enr_load((const void *)SAVE_ADDR);     /* ...unless it was saved in flash */
    mode_reset();
    screen_init();
    log_str(loaded ? "taught words loaded from flash\r\n" : "nothing taught yet: hold PA0 for 1 s\r\n");
    mic_start();
    for (;;) loop_once();
#endif
}
