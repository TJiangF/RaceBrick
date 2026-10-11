#include "gfx.h"
#include "config.h"
#include "font5x7.h"
#include <math.h>

static uint16_t s_fb[LCD_W * LCD_H];

/* view transform */
static int s_dx = 0, s_dy = 0;
static int s_cx0 = 0, s_cy0 = 0, s_cx1 = LCD_W, s_cy1 = LCD_H;
static float s_scale = 1.0f;

void gfx_init(void) {
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = 0;
}
uint16_t *gfx_fb(void) { return s_fb; }

void gfx_reset_view(void) {
    s_dx = 0; s_dy = 0; s_scale = 1.0f;
    s_cx0 = 0; s_cy0 = 0; s_cx1 = LCD_W; s_cy1 = LCD_H;
}
void gfx_set_offset(int dx, int dy) { s_dx = dx; s_dy = dy; }
void gfx_set_scale(float s) { s_scale = s > 0.01f ? s : 1.0f; }
void gfx_set_clip(int x, int y, int w, int h) {
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w; if (s_cx1 > LCD_W) s_cx1 = LCD_W;
    s_cy1 = y + h; if (s_cy1 > LCD_H) s_cy1 = LCD_H;
}

void gfx_clear(uint16_t c) {
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = c;
}

void gfx_pixel(int x, int y, uint16_t c) {
    x = (int)lroundf(x * s_scale) + s_dx;
    y = (int)lroundf((y + s_dy) * s_scale);
    if (x < s_cx0 || x >= s_cx1 || y < s_cy0 || y >= s_cy1) return;
    s_fb[y * LCD_W + x] = c;
}

void gfx_fill(int x, int y, int w, int h, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    int x0 = (int)lroundf(x * s_scale) + s_dx;
    int y0 = (int)lroundf(y * s_scale) + s_dy;
    int x1 = (int)lroundf((x + w) * s_scale) + s_dx;
    int y1 = (int)lroundf((y + h) * s_scale) + s_dy;
    if (x0 < s_cx0) x0 = s_cx0;
    if (y0 < s_cy0) y0 = s_cy0;
    if (x1 > s_cx1) x1 = s_cx1;
    if (y1 > s_cy1) y1 = s_cy1;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &s_fb[yy * LCD_W];
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

void gfx_hline(int x, int y, int w, uint16_t c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, uint16_t c) { gfx_fill(x, y, 1, h, c); }

void gfx_rect(int x, int y, int w, int h, uint16_t c) {
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_rect2(int x, int y, int w, int h, int t, uint16_t c) {
    gfx_fill(x, y, w, t, c);
    gfx_fill(x, y + h - t, w, t, c);
    gfx_fill(x, y, t, h, c);
    gfx_fill(x + w - t, y, t, h, c);
}

void gfx_line(int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int dy = y1 - y0; if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint16_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c); gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx - x, cy - y, c); gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void gfx_disc(int cx, int cy, int r, uint16_t c) {
    for (int y = -r; y <= r; y++) {
        int f = r * r - y * y;
        if (f < 0) continue;
        int w = (int)(sqrtf((float)f) + 0.5f);
        gfx_hline(cx - w, cy + y, 2 * w + 1, c);
    }
}

void gfx_poly_fill(const int *xs, const int *ys, int n, uint16_t c) {
    if (n < 3) return;
    int miny = ys[0], maxy = ys[0];
    for (int i = 1; i < n; i++) {
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int xi[64];
    for (int y = miny; y <= maxy; y++) {
        int nx = 0;
        for (int i = 0; i < n && nx < 63; i++) {
            int j = (i + 1) % n;
            int yi = ys[i], yj = ys[j];
            if ((yi <= y && yj > y) || (yj <= y && yi > y)) {
                int x = xs[i] + (int)((float)(y - yi) / (float)(yj - yi) * (xs[j] - xs[i]));
                xi[nx++] = x;
            }
        }
        for (int a = 0; a < nx; a++)
            for (int b = a + 1; b < nx; b++)
                if (xi[b] < xi[a]) { int t = xi[a]; xi[a] = xi[b]; xi[b] = t; }
        for (int k = 0; k + 1 < nx; k += 2)
            gfx_hline(xi[k], y, xi[k + 1] - xi[k] + 1, c);
    }
}

void gfx_dim(int x, int y, int w, int h, uint16_t c) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            if (((xx + yy) & 1) == 0) gfx_pixel(xx, yy, c);
}

void gfx_pxbox(int x, int y, int w, int h, uint16_t border, int filled, uint16_t fill) {
    if (filled) gfx_fill(x + 2, y + 2, w - 4, h - 4, fill);
    gfx_fill(x + 3, y, w - 6, 2, border);
    gfx_fill(x + 3, y + h - 2, w - 6, 2, border);
    gfx_fill(x, y + 3, 2, h - 6, border);
    gfx_fill(x + w - 2, y + 3, 2, h - 6, border);
    gfx_fill(x + 1, y + 1, 2, 2, border);
    gfx_fill(x + w - 3, y + 1, 2, 2, border);
    gfx_fill(x + 1, y + h - 3, 2, 2, border);
    gfx_fill(x + w - 3, y + h - 3, 2, 2, border);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* scale semantics:
 *   0  -> 1.0x  (raw 5x7, for dense info grids)      cell 5x7   advance 6
 *   1  -> 1.5x  (default readable UI text)           cell 8x11  advance 9
 *   >=2-> Nx    (headlines / big numbers)            cell 5Nx7N advance 6N
 */
static void glyph_metrics(int scale, int *W, int *H, int *adv) {
    if (scale <= 0) { *W = 5; *H = 7; *adv = 6; }
    else if (scale == 1) { *W = 8; *H = 11; *adv = 9; }
    else { *W = 5 * scale; *H = 7 * scale; *adv = 6 * scale; }
}

void gfx_text(int x, int y, const char *s, uint16_t c, int scale) {
    int W, H, adv;
    glyph_metrics(scale, &W, &H, &adv);
    int cx = x;
    for (; *s; s++) {
        char ch = up(*s);
        if (ch == ' ') { cx += adv; continue; }
        int idx = ch - 32;
        if (idx < 0 || idx >= 96) { cx += adv; continue; }
        const uint8_t *g = FONT5X7[idx];
        if (scale <= 0) {
            for (int r = 0; r < 7; r++) {
                uint8_t bits = g[r];
                if (!bits) continue;
                for (int col = 0; col < 5; col++)
                    if (bits & (1 << (4 - col))) gfx_pixel(cx + col, y + r, c);
            }
        } else if (scale == 1) {
            for (int ty = 0; ty < H; ty++) {
                uint8_t bits = g[ty * 7 / H];
                if (!bits) continue;
                for (int tx = 0; tx < W; tx++)
                    if (bits & (1 << (4 - (tx * 5 / W)))) gfx_pixel(cx + tx, y + ty, c);
            }
        } else {
            for (int r = 0; r < 7; r++) {
                uint8_t bits = g[r];
                if (!bits) continue;
                for (int col = 0; col < 5; col++) {
                    if (bits & (1 << (4 - col))) {
                        gfx_fill(cx + col * scale, y + r * scale, scale, scale, c);
                    }
                }
            }
        }
        cx += adv;
    }
}

int gfx_text_w(const char *s, int scale) {
    int W, H, adv;
    glyph_metrics(scale, &W, &H, &adv);
    (void)W; (void)H;
    int n = 0;
    for (; *s; s++) n++;
    return n * adv;
}

void gfx_text_center(int cx, int y, const char *s, uint16_t c, int scale) {
    gfx_text(cx - gfx_text_w(s, scale) / 2, y, s, c, scale);
}
void gfx_text_right(int rx, int y, const char *s, uint16_t c, int scale) {
    gfx_text(rx - gfx_text_w(s, scale), y, s, c, scale);
}
