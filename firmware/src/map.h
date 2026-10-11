/* Pixel track-map renderer + small icon painters (port of js/map.js).
 * Map coordinates are in the CONTENT-local space (see app.h).
 */
#pragma once
#include <stdint.h>
#include "sim.h"

typedef struct {
    int vx, vy, vw, vh;          /* viewport in content-local coords */
    const track_t *track;
    float scale, ox, oy;         /* current transform */
    float tScale, tOx, tOy;      /* eased target transform */
    bool has;
} tmap_t;

void map_init(tmap_t *m, int x, int y, int w, int h);
void map_set_track(tmap_t *m, const track_t *t);
void map_fit_points(tmap_t *m, const pt_t *pts, int n, float minSpan);
void map_fit_engine(tmap_t *m, const engine_t *e, float minSpan);
void map_smooth(tmap_t *m, float dt, float rate);
pt_t map_px(const tmap_t *m, pt_t world);

void map_begin(tmap_t *m);
void map_centerline(tmap_t *m, uint16_t color, int step);
void map_trace(tmap_t *m, const pt_t *pts, int n, uint16_t color, int size);
void map_trail(tmap_t *m, const engine_t *e);
void map_start_line(tmap_t *m, float f, uint16_t color);
void map_tick(tmap_t *m, float f, uint16_t color, int len);
void map_marker(tmap_t *m, float f, uint16_t color, int size, bool blinkOn);
void map_car(tmap_t *m, float f, uint16_t color);
void map_car_at(tmap_t *m, pt_t p, pt_t tangent, uint16_t color);
void map_start_mark_at(tmap_t *m, pt_t p, pt_t tangent, uint16_t color);
void map_marker_at(tmap_t *m, pt_t p, uint16_t color, int size, bool blinkOn);
void map_jitter_dot(tmap_t *m, float seed, uint16_t color);

void map_draw_thumb(int x, int y, int w, int h, const track_t *t, uint16_t color);
void map_draw_gmeter(int x, int y, int w, int h, float lat, float lng,
                     const gpt_t *trail, int n, float maxG);
void map_draw_vehicle(int x, int y, int w, int h, uint16_t body, uint16_t yellow);
