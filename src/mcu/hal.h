/* Boundary between portable code and the board.  Implemented by board.c on the
 * STM32 and by tests/host/sim.c on a PC. */
#ifndef HAL_H
#define HAL_H
#include <stdint.h>
#include <stdbool.h>

uint32_t hal_millis(void);
void     hal_fpga_send(const uint8_t *buf, uint32_t n);   /* one CS-framed SPI packet */
void     hal_rtc_get(uint8_t *h, uint8_t *m, uint8_t *s);
void     hal_rtc_set(uint8_t h, uint8_t m, uint8_t s);
void     hal_store_read(void *dst, uint32_t n);            /* raw copy from flash      */
bool     hal_store_write(const void *src, uint32_t n);     /* erase + program          */
void     hal_audio_flush(void);                            /* drop queued frames       */
void     hal_log(const char *s);

#endif
