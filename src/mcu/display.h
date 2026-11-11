/* MCU side of the FPGA display link.  Each call is one CS-framed SPI packet:
 *   01                    clear the whole framebuffer
 *   02 x cfg chars...     draw text at column x; cfg = page[2:0] | scale[5:4]
 *   03 mask               pages that blink (bit n = page n)
 *   04 b                  invert display
 *   05 x page bytes...    raw column bytes (bit 0 = top pixel) for bitmaps
 * Glyphs are 6 px wide x 8 px tall, multiplied by scale (1..3).  A page is 8 rows. */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdint.h>

enum { DISP_LEFT, DISP_CENTER };

void disp_clear(void);
void disp_text(int x, int page, int scale, const char *s);
void disp_line(int page, int scale, const char *s, int align);  /* whole row, padded */
void disp_blink(uint8_t page_mask);
void disp_invert(int on);
void disp_raw(int x, int page, const uint8_t *cols, int n);

#endif
