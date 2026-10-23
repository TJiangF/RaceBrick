/* Tiny pixel graphics library: RGB565 framebuffer + primitives + 5x7 font. */
#pragma once
#include <stdint.h>

void gfx_init(void);
uint16_t *gfx_fb(void);

/* view transform: clip rect is in screen space, offset translates every draw */
void gfx_reset_view(void);
void gfx_set_offset(int dx, int dy);
void gfx_set_clip(int x, int y, int w, int h);
/* global visual scale (used to fit the 360x240 design into a 320x240 panel) */
void gfx_set_scale(float s);

void gfx_clear(uint16_t c);
void gfx_pixel(int x, int y, uint16_t c);
/* Copy a raw RGB565 image into the framebuffer (scale must be 1). */
void gfx_blit(int x, int y, const uint16_t *src, int w, int h, int stride);
void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);      /* 1px outline */
void gfx_rect2(int x, int y, int w, int h, int t, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_circle(int cx, int cy, int r, uint16_t c);         /* outline */
void gfx_disc(int cx, int cy, int r, uint16_t c);
/* darken a region with a 50% checkerboard (used for modal backdrops) */
void gfx_dim(int x, int y, int w, int h, uint16_t c);

/* filled polygon (scanline). xs/ys are screen-space vertices, n >= 3 */
void gfx_poly_fill(const int *xs, const int *ys, int n, uint16_t c);

/* pixel-corner box (web .pxbox): 2px border, 3px stepped corners */
void gfx_pxbox(int x, int y, int w, int h, uint16_t border, int filled, uint16_t fill);

/* text (uppercased), integer scale */
void gfx_text(int x, int y, const char *s, uint16_t c, int scale);
int  gfx_text_w(const char *s, int scale);
void gfx_text_center(int cx, int y, const char *s, uint16_t c, int scale);
void gfx_text_right(int rx, int y, const char *s, uint16_t c, int scale);
