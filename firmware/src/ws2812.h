/* WS2812 (NeoPixel) strip driver over RMT. 10 LEDs, GRB order. */
#pragma once
#include <stdint.h>

void ws2812_init(void);
void ws2812_set_brightness(int percent);          /* 0..100 global scale */
void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b); /* unscaled color */
void ws2812_show(void);                            /* push the frame */
void ws2812_clear(void);                           /* all off */
