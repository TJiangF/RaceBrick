#pragma once
#include <stdint.h>

void lcd_init(void);
void lcd_flush(const uint16_t *fb);
void lcd_set_brightness(int percent); /* 0..100 */
void lcd_reconfigure(int swap_xy, int mirror_x, int mirror_y, int invert);
void lcd_set_gap(int x, int y);
