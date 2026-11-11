#include <string.h>
#include "display.h"
#include "hal.h"

void disp_clear(void) { uint8_t b = 0x01; hal_fpga_send(&b, 1); }
void disp_blink(uint8_t mask) { uint8_t b[2] = { 0x03, mask }; hal_fpga_send(b, 2); }
void disp_invert(int on) { uint8_t b[2] = { 0x04, (uint8_t)(on != 0) }; hal_fpga_send(b, 2); }

void disp_text(int x, int page, int scale, const char *s)
{
    uint8_t b[3 + 24];
    int n = 0;
    b[0] = 0x02;
    b[1] = (uint8_t)x;
    b[2] = (uint8_t)((page & 7) | ((scale & 3) << 4));
    while (s[n] && n < 24) { b[3 + n] = (uint8_t)s[n]; n++; }
    hal_fpga_send(b, (uint32_t)(3 + n));
}

void disp_line(int page, int scale, const char *s, int align)
{
    int cols = 128 / (6 * scale);                 /* 21, 10 or 7 characters */
    int len = (int)strlen(s);
    char buf[22];
    if (len > cols) len = cols;
    int pad = (align == DISP_CENTER) ? (cols - len) / 2 : 0;
    memset(buf, ' ', (size_t)cols);
    memcpy(buf + pad, s, (size_t)len);
    buf[cols] = 0;
    disp_text((128 - cols * 6 * scale) / 2, page, scale, buf);
}

void disp_raw(int x, int page, const uint8_t *cols, int n)
{
    uint8_t b[3 + 32];
    if (n > 32) n = 32;
    b[0] = 0x05; b[1] = (uint8_t)x; b[2] = (uint8_t)(page & 7);
    memcpy(b + 3, cols, (size_t)n);
    hal_fpga_send(b, (uint32_t)(3 + n));
}
