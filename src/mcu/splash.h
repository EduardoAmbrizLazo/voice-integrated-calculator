/* splash.h - start-up animation for the OLED (about 2.2 seconds)
 *
 *   1. a voice waveform of vertical bars swells and settles into a flat line
 *   2. "VOICE" and "CALCULATOR" type themselves in
 *   3. a progress bar fills along the bottom
 *
 * It only uses commands the FPGA renderer already has: clear (0x01), text (0x02) and raw
 * pixel columns (0x05), so the FPGA design does not change. No hardware in here: the two
 * functions below are supplied by the program that uses it (see midpointmain.c), which also
 * lets the animation be previewed on a PC.
 */
#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>

/* supplied by the caller */
void     splash_send(const uint8_t *buf, int n);     /* send one SPI packet to the FPGA */
uint32_t splash_ms(void);                            /* milliseconds, counting up       */

/* plays the animation, then returns (the screen is left showing the finished splash) */
void splash_run(void);

#endif
