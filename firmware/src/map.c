#include "map.h"
#include "gfx.h"
#include "theme.h"
#include "util.h"
#include "track_img.h"
#include <math.h>
#include <string.h>

#define GRID 8

void map_init(tmap_t *m, int x, int y, int w, int h) {
    m->vx = x; m->vy = y; m->vw = w; m->vh = h;
    m->track = NULL;
    m->scale = 1; m->ox = 0; m->oy = 0;
    m->tScale = 1; m->tOx = 0; m->tOy = 0;
    m->has = false;
}

static void blk(tmap_t *m, float x, float y, int size, uint16_t c) {
    int qx = (int)lroundf(x / 2.0f) * 2;
    int qy = (int)lroundf(y / 2.0f) * 2;
    gfx_fill(m->vx + qx, m->vy + qy, size, size, c);
}

static void map_commit(tmap_t *m) {
    m->scale = m->tScale; m->ox = m->tOx; m->oy = m->tOy; m->has = true;
}

pt_t map_px(const tmap_t *m, pt_t world) {
    pt_t r = {world.x * m->scale + m->ox, world.y * m->scale + m->oy};
    /* GPS y is north-positive but screen y grows downward: flip so north is up
     * (the fit is centred, so flipping about the viewport centre keeps it fitted) */
    r.y = (float)m->vh - r.y;
    return r;
}

static pt_t point_px(const tmap_t *m, float f) {
    return map_px(m, track_point_at(m->track, f));
}
static pt_t tangent_px(const tmap_t *m, float f) {
    pt_t a = point_px(m, f - 0.004f);
    pt_t b = point_px(m, f + 0.004f);
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = hypotf(dx, dy);
    if (len < 1e-6f) len = 1e-6f;
    pt_t r = {dx / len, dy / len};
    return r;
}

void map_set_track(tmap_t *m, const track_t *t) {
    m->track = t;
    map_fit_points(m, t->points, t->N, 0);
    map_commit(m);
}

static void fit(tmap_t *m, const pt_t *points, int n, float minSpan) {
    int W = m->vw, H = m->vh, pad = 18;
    if (!points || n == 0) {
        float ms = minSpan > 0 ? minSpan : 0.25f;
        m->tScale = ((W < H ? W : H) - 2 * pad) / ms;
        m->tOx = W / 2.0f - 0.5f * m->tScale;
        m->tOy = H / 2.0f - 0.5f * m->tScale;
        return;
    }
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < n; i++) {
        if (points[i].x < minX) minX = points[i].x;
        if (points[i].x > maxX) maxX = points[i].x;
        if (points[i].y < minY) minY = points[i].y;
        if (points[i].y > maxY) maxY = points[i].y;
    }
    float spanX = maxX - minX, spanY = maxY - minY;
    float ms = minSpan;
    if (spanX < ms) { float c = (minX + maxX) / 2; minX = c - ms / 2; spanX = ms; }
    if (spanY < ms) { float c = (minY + maxY) / 2; minY = c - ms / 2; spanY = ms; }
    float s = (W - 2 * pad) / (spanX > 0 ? spanX : 1);
    float sy = (H - 2 * pad) / (spanY > 0 ? spanY : 1);
    if (sy < s) s = sy;
    m->tScale = s;
    m->tOx = (W - spanX * s) / 2 - minX * s;
    m->tOy = (H - spanY * s) / 2 - minY * s;
}

void map_fit_points(tmap_t *m, const pt_t *pts, int n, float minSpan) {
    fit(m, pts, n, minSpan);
    if (!m->has) map_commit(m);
}

#define FITBUF 1200
static pt_t s_fitbuf[FITBUF];
void map_fit_engine(tmap_t *m, const engine_t *e, float minSpan) {
    int n = 0;
    for (int l = 0; l < e->nLaps && n < FITBUF; l++)
        for (int i = 0; i < e->laps[l].nTrace && n < FITBUF; i++)
            s_fitbuf[n++] = e->laps[l].trace[i];
    for (int i = 0; i < e->nTrace && n < FITBUF; i++)
        s_fitbuf[n++] = e->lapTrace[i];
    fit(m, s_fitbuf, n, minSpan);
}

void map_smooth(tmap_t *m, float dt, float rate) {
    if (!m->has) { map_commit(m); return; }
    float k = 1.0f - expf(-(rate > 0 ? rate : 5.0f) * dt);
    m->scale += (m->tScale - m->scale) * k;
    m->ox += (m->tOx - m->ox) * k;
    m->oy += (m->tOy - m->oy) * k;
}


static uint16_t blend50(uint16_t a, uint16_t b) {
    int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    return (uint16_t)((((ar + br) / 2) << 11) |
                      (((ag + bg) / 2) << 5) |
                      ((ab + bb) / 2));
}

static bool sat_draw_view(int x, int y, int w, int h, const track_t *t,
                          float scale, float ox, float oy) {
    if (!t || t->store_slot < 0 || scale <= 1e-6f) return false;
    const uint16_t *img = NULL;
    int iw = 0, ih = 0;
    float x0, y0, x1, y1;
    if (!track_img_get(t->store_slot, &img, &iw, &ih, &x0, &y0, &x1, &y1) ||
        !img || iw <= 0 || ih <= 0) return false;
    float inv = 1.0f / scale;
    float bw = 1.0f / (x1 - x0);
    float bh = 1.0f / (y1 - y0);
    bool any = false;
    for (int sy = 0; sy < h; sy++) {
        float wy = ((float)h - (sy + 0.5f) - oy) * inv;
        if (wy < y0 || wy > y1) continue;
        int v = (int)((y1 - wy) * bh * (float)(ih - 1) + 0.5f);
        if (v < 0) v = 0;
        else if (v >= ih) v = ih - 1;
        const uint16_t *row = img + (size_t)v * iw;
        for (int sx = 0; sx < w; sx++) {
            float wx = ((sx + 0.5f) - ox) * inv;
            if (wx < x0 || wx > x1) continue;
            int u = (int)((wx - x0) * bw * (float)(iw - 1) + 0.5f);
            if (u < 0) u = 0;
            else if (u >= iw) u = iw - 1;
            gfx_pixel(x + sx, y + sy, blend50(row[u], PAL.mapbg));
            any = true;
        }
    }
    return any;
}

void map_begin(tmap_t *m) {
    gfx_fill(m->vx, m->vy, m->vw, m->vh, PAL.mapbg);
    bool has_sat = sat_draw_view(m->vx, m->vy, m->vw, m->vh, m->track,
                                 m->scale, m->ox, m->oy);
    if (!has_sat) {
        for (int y = GRID; y < m->vh; y += GRID)
            for (int x = GRID; x < m->vw; x += GRID)
                gfx_pixel(m->vx + x, m->vy + y, PAL.mapgrid);
    }
    uint16_t b = PAL.mapborder;
    gfx_hline(m->vx, m->vy, m->vw, b);
    gfx_hline(m->vx, m->vy + m->vh - 1, m->vw, b);
    gfx_vline(m->vx, m->vy, m->vh, b);
    gfx_vline(m->vx + m->vw - 1, m->vy, m->vh, b);
}

void map_track_ribbon(tmap_t *m, float width_m, uint16_t color) {
    if (!m->track) return;
    const track_t *t = m->track;
    float total = t->total;
    if (total <= 1e-6f) total = t->lengthM;
    if (total <= 1e-6f) return;
    float wnorm = width_m * total / t->lengthM;
    float half = wnorm * m->scale * 0.5f;
    if (half < 1.0f) half = 1.0f;
    int N = t->N;
    for (int i = 0; i < N; i++) {
        pt_t a = map_px(m, t->points[i]);
        pt_t b = map_px(m, t->points[(i + 1) % N]);
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = hypotf(dx, dy);
        if (len < 1e-4f) continue;
        float nx = -dy / len, ny = dx / len;
        int xs[4] = {(int)(m->vx + a.x + nx * half), (int)(m->vx + a.x - nx * half),
                     (int)(m->vx + b.x - nx * half), (int)(m->vx + b.x + nx * half)};
        int ys[4] = {(int)(m->vy + a.y + ny * half), (int)(m->vy + a.y - ny * half),
                     (int)(m->vy + b.y - ny * half), (int)(m->vy + b.y + ny * half)};
        gfx_poly_fill(xs, ys, 4, color);
    }
    for (int i = 0; i < N; i += 2) {
        pt_t p = map_px(m, t->points[i]);
        gfx_disc((int)(m->vx + p.x), (int)(m->vy + p.y), (int)half, color);
    }
}

void map_start_checker_at(tmap_t *m, pt_t pworld, pt_t tangent, uint16_t color) {
    pt_t p = map_px(m, pworld);
    /* world tangent -> screen tangent (y flipped), then screen normal. */
    float tx = tangent.x, ty = -tangent.y;
    float nx = -ty, ny = tx;
    const int half = 24;
    const int cell = 4;
    for (int d = -half; d < half; d += cell) {
        for (int r = 0; r < 2; r++) {
            int px = (int)(m->vx + p.x + nx * d + tx * r * cell);
            int py = (int)(m->vy + p.y + ny * d + ty * r * cell);
            uint16_t c = (((d / cell) + r) & 1) ? color : PAL.white;
            gfx_fill(px, py, cell, cell, c);
        }
    }
}

void map_start_checker_width(tmap_t *m, pt_t pworld, pt_t tangent,
                                float width_m, uint16_t color) {
    pt_t p = map_px(m, pworld);
    float tx = tangent.x, ty = -tangent.y;
    float nx = -ty, ny = tx;
    float half = 12.0f;
    if (m->track && m->track->nscale > 1e-9f && width_m > 0.0f)
        half = width_m * m->track->nscale * m->scale * 0.5f;
    int hp = (int)lroundf(half);
    if (hp < 2) hp = 2;
    const int cell = 4;
    for (int d = -hp; d < hp; d += cell) {
        for (int r = 0; r < 2; r++) {
            int px = (int)(m->vx + p.x + nx * d + tx * r * cell);
            int py = (int)(m->vy + p.y + ny * d + ty * r * cell);
            uint16_t c = (((d / cell) + r) & 1) ? color : PAL.white;
            gfx_fill(px, py, cell, cell, c);
        }
    }
}

void map_start_arrow_at(tmap_t *m, pt_t pworld, pt_t tangent, uint16_t color) {
    pt_t p = map_px(m, pworld);
    float tx = tangent.x, ty = -tangent.y;
    float len = hypotf(tx, ty);
    if (len < 1e-5f) return;
    tx /= len; ty /= len;
    float nx = -ty, ny = tx;
    int tipx = (int)(m->vx + p.x + tx * 17), tipy = (int)(m->vy + p.y + ty * 17);
    int bx = (int)(m->vx + p.x - tx * 8), by = (int)(m->vy + p.y - ty * 8);
    int lx = (int)(bx + nx * 9), ly = (int)(by + ny * 9);
    int rx = (int)(bx - nx * 9), ry = (int)(by - ny * 9);
    int xs[3] = {tipx, lx, rx};
    int ys[3] = {tipy, ly, ry};
    gfx_poly_fill(xs, ys, 3, color);
    gfx_line(tipx, tipy, lx, ly, PAL.white);
    gfx_line(tipx, tipy, rx, ry, PAL.white);
}

void map_centerline(tmap_t *m, uint16_t color, int step) {
    if (!m->track) return;
    if (step < 1) step = 2;
    for (int i = 0; i < m->track->N; i += step) {
        pt_t p = map_px(m, m->track->points[i]);
        blk(m, p.x, p.y, 2, color);
    }
}

void map_trace(tmap_t *m, const pt_t *pts, int n, uint16_t color, int size) {
    int s = size > 0 ? size : 2;
    for (int i = 0; i < n; i++) {
        pt_t q = map_px(m, pts[i]);
        blk(m, q.x, q.y, s, color);
    }
}

void map_trail(tmap_t *m, const engine_t *e) {
    for (int l = 0; l < e->nLaps; l++)
        map_trace(m, e->laps[l].trace, e->laps[l].nTrace, PAL.histtrace, 2);
    map_trace(m, e->lapTrace, e->nTrace, PAL.curtrace, 2);
}

void map_start_line(tmap_t *m, float f, uint16_t color) {
    if (!m->track) return;
    pt_t p = point_px(m, f);
    pt_t t = tangent_px(m, f);
    float nx = -t.y, ny = t.x;
    for (int d = -9; d <= 9; d += 2) blk(m, p.x + nx * d, p.y + ny * d, 2, color);
}

void map_tick(tmap_t *m, float f, uint16_t color, int len) {
    if (!m->track) return;
    pt_t p = point_px(m, f);
    pt_t t = tangent_px(m, f);
    float nx = -t.y, ny = t.x;
    int L = len > 0 ? len : 5;
    for (int d = -L; d <= L; d += 2) blk(m, p.x + nx * d, p.y + ny * d, 2, color);
}

void map_marker(tmap_t *m, float f, uint16_t color, int size, bool blinkOn) {
    if (!m->track || !blinkOn) return;
    pt_t p = point_px(m, f);
    int s = size > 0 ? size : 4;
    blk(m, p.x - 1, p.y - 1, s, color);
    blk(m, p.x - 3, p.y - 1, 2, color);
    blk(m, p.x + 1, p.y - 1, 2, color);
}

void map_car(tmap_t *m, float f, uint16_t color) {
    if (!m->track) return;
    map_car_at(m, track_point_at(m->track, f), track_tangent_at(m->track, f), color);
}

void map_car_at(tmap_t *m, pt_t pworld, pt_t tangent, uint16_t color) {
    pt_t p = map_px(m, pworld);
    /* tangent is world-space (Y north); screen Y is flipped. */
    blk(m, p.x + tangent.x * 5 - 1, p.y - tangent.y * 5 - 1, 2, color);
    blk(m, p.x - 2, p.y - 2, 4, PAL.white);
    blk(m, p.x - 1, p.y - 1, 2, color);
}

void map_start_mark_at(tmap_t *m, pt_t pworld, pt_t tangent, uint16_t color) {
    pt_t p = map_px(m, pworld);
    /* World tangent -> screen perpendicular: (ty, tx), not (-ty, tx). */
    float nx = tangent.y, ny = tangent.x;
    for (int d = -9; d <= 9; d += 2) blk(m, p.x + nx * d, p.y + ny * d, 2, color);
}

void map_marker_at(tmap_t *m, pt_t pworld, uint16_t color, int size, bool blinkOn) {
    if (!blinkOn) return;
    pt_t p = map_px(m, pworld);
    int s = size > 0 ? size : 4;
    blk(m, p.x - 1, p.y - 1, s, color);
    blk(m, p.x - 3, p.y - 1, 2, color);
    blk(m, p.x + 1, p.y - 1, 2, color);
}

void map_jitter_dot(tmap_t *m, float seed, uint16_t color) {
    float cx = m->vw * 0.5f, cy = m->vh * 0.52f;
    float jx = sinf(seed * 7.3f) * 3 + sinf(seed * 2.1f) * 2;
    float jy = cosf(seed * 5.7f) * 3 + cosf(seed * 1.7f) * 2;
    blk(m, cx + jx, cy + jy, 4, color);
}

/* ---------- thumbnails / meters / icons ---------- */

void map_draw_thumb(int x, int y, int w, int h, const track_t *t, uint16_t color) {
    gfx_fill(x, y, w, h, PAL.mapbg);
    if (!t) return;
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < t->N; i++) {
        if (t->points[i].x < minX) minX = t->points[i].x;
        if (t->points[i].x > maxX) maxX = t->points[i].x;
        if (t->points[i].y < minY) minY = t->points[i].y;
        if (t->points[i].y > maxY) maxY = t->points[i].y;
    }
    int pad = 4;
    float rx = maxX - minX; if (rx == 0) rx = 1;
    float ry = maxY - minY; if (ry == 0) ry = 1;
    float s = (w - 2 * pad) / rx, sy = (h - 2 * pad) / ry;
    if (sy < s) s = sy;
    float ox = (w - rx * s) / 2 - minX * s;
    float oy = (h - ry * s) / 2 - minY * s;
    sat_draw_view(x, y, w, h, t, s, ox, oy);
    for (int i = 0; i < t->N; i += 4) {
        int px = x + (int)lroundf(t->points[i].x * s + ox);
        int py = y + h - (int)lroundf(t->points[i].y * s + oy);   /* north up */
        gfx_pixel(px, py, color);
    }
}

void map_draw_gmeter(int x, int y, int w, int h, float lat, float lng,
                     const gpt_t *trail, int n, float maxG) {
    /* Transparent: the page background is the instrument backdrop. */
    float cx = x + w / 2.0f, cy = y + h / 2.0f;
    float R = (w < h ? w : h) / 2.0f - 6;
    float mx = maxG > 0 ? maxG : 2.0f;
    for (float g = 0.5f; g <= mx + 0.001f; g += 0.5f)
        gfx_circle((int)cx, (int)cy, (int)((g / mx) * R), PAL.gmring);
    gfx_line((int)(cx - R), (int)cy, (int)(cx + R), (int)cy, PAL.gmring);
    gfx_line((int)cx, (int)(cy - R), (int)cx, (int)(cy + R), PAL.gmring);
    for (int i = 0; i < n; i++) {
        float qx = cx + (trail[i].l / mx) * R;
        float qy = cy - (trail[i].n / mx) * R;
        gfx_pixel((int)lroundf(qx), (int)lroundf(qy), PAL.gmtrail);
    }
    {
        float qx = cx + (lat / mx) * R;
        float qy = cy - (lng / mx) * R;
        float mag = hypotf(lat, lng);
        uint16_t col = mag >= 1.0f ? PAL.red : (mag >= 0.5f ? PAL.yellow : PAL.green);
        gfx_fill((int)lroundf(qx) - 3, (int)lroundf(qy) - 3, 7, 7, col);
        gfx_pixel((int)lroundf(qx), (int)lroundf(qy), PAL.mapbg);
    }
    gfx_disc((int)cx, (int)cy, 5, PAL.red);
    gfx_disc((int)cx, (int)cy, 2, PAL.white);
}

void map_draw_vehicle(int x, int y, int w, int h, uint16_t body, uint16_t yellow) {
    uint16_t dark = RGB565(0x0a, 0x0a, 0x0c);
    int px = x, py = y;
    gfx_fill(px + 4, py + h - 14, 10, 12, dark);
    gfx_fill(px + w - 14, py + h - 14, 10, 12, dark);
    gfx_fill(px + 6, py + h - 22, w - 12, 14, body);
    gfx_fill(px + 10, py + h - 28, w - 20, 8, body);
    gfx_fill(px + 6, py + h - 22, w - 12, 3, RGB565(0x3a, 0x3a, 0x42));
    gfx_fill(px + w / 2 - 8, py + h - 27, 16, 12, dark);
    gfx_fill(px + w / 2 - 4, py + h - 26, 8, 7, body);
    gfx_fill(px + 11, py + h - 24, 4, 4, yellow);
    gfx_fill(px + w - 15, py + h - 24, 4, 4, yellow);
}
