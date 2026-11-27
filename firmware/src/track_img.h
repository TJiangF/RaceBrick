#pragma once
#include <stdbool.h>
#include <stdint.h>

bool track_img_save(int track_slot, const uint8_t *pal16, int len,
                    float x0, float y0, float x1, float y1);
bool track_img_clear(int track_slot);
bool track_img_get(int track_slot, const uint16_t **pixels, int *w, int *h,
                   float *x0, float *y0, float *x1, float *y1);
void track_img_invalidate(void);
