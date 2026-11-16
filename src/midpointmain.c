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
 */

#include <stdint.h>
#include "stm32l432xx.h"

#define STEP 2          /* 1 = no microphone: just prove MCU -> FPGA -> OLED (shows HELLO)
                           2 = the checkoff demo                                          */

#define ON_MARGIN   27  /* a slice is "loud" this far above the noise floor (0.376 dB each) */
#define OFF_MARGIN  16  /* and "quiet" when it falls back below this                        */
#define I2S_SLOT    0   /* 0 = mic SEL tied to GND, 1 = SEL tied to 3V3                     */
#define DEBUG       1   /* 1 = show raw microphone numbers on rows 1 and 5 (see bottom of file) */

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

static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; *p = 0; return p; }
static char *put_int(char *p, uint32_t v)
{
    char t[10]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0; return p;
}

static char *put_hex5(char *p, uint32_t v)
{
    for (int s = 16; s >= 0; s -= 4) *p++ = "0123456789ABCDEF"[(v >> s) & 15];
    *p = 0; return p;
}

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

static unsigned shown_sec = 99;
static void draw_clock(void)                            /* top row: title and HH:MM:SS */
{
    unsigned h, m, sec;
    char b[24], *p;
    rtc_get(&h, &m, &sec);
    if (sec == shown_sec) return;
    shown_sec = sec;
    p = put_str(b, "MIDPOINT    ");
    p = put_2d(p, h); *p++ = ':'; p = put_2d(p, m); *p++ = ':'; put_2d(p, sec);
    disp_line(0, 1, b, 0);
}

/* --------------------------------------------------------------- microphone */
#define HALF_FRAMES 512                         /* I2S frames per DMA half (8.2 ms)  */
static int32_t dma_buf[2 * 2 * HALF_FRAMES];    /* left, right, left, right ...      */

static int32_t  dc;                             /* running average (the mic has a DC offset) */
static volatile uint32_t dbg_irq, dbg_l, dbg_r, dbg_e;     /* for the DEBUG rows */
static uint64_t acc;                            /* energy collected in this 16 ms slice      */
static uint8_t  halves;
static volatile uint8_t  q[16];                 /* loudness of finished slices       */
static volatile uint8_t  q_head, q_tail;

/* 8 x log2(v): loudness on a log scale, 8 counts per doubling of energy */
static int log8(uint64_t v)
{
    if (!v) return 0;
    int p = 63 - __builtin_clzll(v);
    uint32_t frac = (p >= 3) ? (uint32_t)((v >> (p - 3)) & 7) : (uint32_t)((v << (3 - p)) & 7);
    return 8 * p + (int)frac;
}

/* Called for each half of the DMA buffer as it fills. */
static void audio_half(const int32_t *w)
{
    dbg_irq++;
    dbg_l = ((uint32_t)w[0] >> 14) & 0x3FFFF;   /* one raw sample from each channel */
    dbg_r = ((uint32_t)w[1] >> 14) & 0x3FFFF;
    for (int i = 0; i < HALF_FRAMES; i += 4) {
        int32_t x = 0;
        for (int k = 0; k < 4; k++) x += w[2 * (i + k) + I2S_SLOT] >> 14;   /* 18-bit samples */
        x >>= 2;                                /* average of 4: 62.5 kHz -> 15.6 kHz */
        dc += (x - dc) >> 7;                    /* remove the DC offset               */
        x -= dc;
        acc += (uint64_t)((int64_t)x * x);
    }
    if (++halves < 2) return;
    halves = 0;
    int e = log8(acc);
    acc = 0;
    dbg_e = (uint32_t)e;
    uint8_t next = (uint8_t)((q_head + 1) & 15);
    if (next != q_tail) { q[q_head] = (uint8_t)(e > 255 ? 255 : e); q_head = next; }
}

void DMA2_Channel1_IRQHandler(void)
{
    uint32_t isr = DMA2->ISR;
    if (isr & DMA_ISR_HTIF1) { DMA2->IFCR = DMA_IFCR_CHTIF1; audio_half(&dma_buf[0]); }
    if (isr & DMA_ISR_TCIF1) { DMA2->IFCR = DMA_IFCR_CTCIF1; audio_half(&dma_buf[2 * HALF_FRAMES]); }
}

/* ------------------------------------------------------------ word detector */
static int      floor_q4 = -1;                  /* noise floor x16                    */
static int      in_word, loud_run, quiet_run, frames, peak;
static uint32_t words, last_ms, t_meter;
static char     big[12] = "READY";

static void draw(const char *note)
{
    char b[24], *p;
    disp_line(2, 3, big, 1);
    if (words) { p = put_str(b, "LENGTH "); p = put_int(p, last_ms); put_str(p, " MS"); }
    else b[0] = 0;
    disp_line(6, 1, note ? note : b, 0);
}

static void on_slice(int e)                     /* e = loudness of one 16 ms slice    */
{
    if (floor_q4 < 0) floor_q4 = e << 4;
    int fl = floor_q4 >> 4, loud = e > fl + ON_MARGIN, quiet = e < fl + OFF_MARGIN;
    if (e - fl > peak) peak = e - fl;

    if (!in_word) {
        if (loud) {
            if (++loud_run >= 3) { in_word = 1; frames = loud_run; quiet_run = 0; }
        } else {
            loud_run = 0;                       /* follow the room: down fast, up slowly */
            floor_q4 += ((e << 4) - floor_q4) / (((e << 4) < floor_q4) ? 4 : 32);
        }
        return;
    }
    frames++;
    quiet_run = quiet ? quiet_run + 1 : 0;
    if (frames > 60) floor_q4 += 8;             /* stuck "loud": the room got noisier */
    if (quiet_run < 8) return;

    in_word = 0; loud_run = 0;
    if (frames - 8 > 50) { draw("TOO LONG - TRY AGAIN"); log_str("sound too long\r\n"); return; }
    char b[32], *p;
    words++;
    last_ms = (uint32_t)(frames - 8) * 16;
    p = put_str(big, "WORD "); put_int(p, words);
    draw(0);
    p = put_str(b, "WORD "); p = put_int(p, words); p = put_str(p, "  ms="); p = put_int(p, last_ms);
    put_str(p, "\r\n"); log_str(b);
}

static void screen_init(void)
{
    disp_clear();
    shown_sec = 99;
    draw_clock();
    disp_line(1, 1, "SAY A WORD", 0);
    draw(0);
}

static void loop_once(void)
{
    while (q_tail != q_head) {
        int e = q[q_tail];
        q_tail = (uint8_t)((q_tail + 1) & 15);
        on_slice(e);
    }
    if (g_ms - t_meter >= 200) {                /* clock and level meter, 5 times a second */
        draw_clock();
        char b[24], *p = put_str(b, "MIC ");
        int bars = peak / 6;
        if (bars > 16) bars = 16;
        while (bars-- > 0) *p++ = '#';
        *p = 0;
        disp_line(7, 1, b, 0);
#if DEBUG
        p = put_str(b, "L "); p = put_hex5(p, dbg_l); p = put_str(p, "  R "); put_hex5(p, dbg_r);
        disp_line(1, 1, b, 0);
        p = put_str(b, "IRQ "); p = put_int(p, dbg_irq % 1000); p = put_str(p, " E "); p = put_int(p, dbg_e);
        p = put_str(p, " F "); put_int(p, (uint32_t)(floor_q4 < 0 ? 0 : floor_q4 >> 4));
        disp_line(5, 1, b, 0);
#endif
        peak = 0;
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
    screen_init();
    mic_start();
    for (;;) loop_once();
#endif
}
