/* All UI screens + the state machine handlers (port of js/screens.js).
 * Rendering uses CONTENT-local coordinates: (0,0) is the top-left of the
 * 360x200 content area (the shell adds the LED/status chrome).
 */
#include "app.h"
#include "gfx.h"
#include "map.h"
#include "theme.h"
#include "util.h"
#include "lcd.h"
#include "gps.h"
#include "ws2812.h"
#include "buzzer.h"
#include "mpu6050.h"
#include "rt.h"
#include "china_map.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CW LCD_W
#define CH CONTENT_H

static tmap_t s_map;
static bool   s_map_ready;

static const char *RACE_PANELS[4] = {"TRACK", "TIMER", "DASH", "SECTOR"};

static const char *VEH_NAMES[4] = {"KART", "4-STROKE", "KART+", "RACE"};
static const uint16_t VEH_COLORS[4] = {
    RGB565(0xff, 0x6b, 0x6b), RGB565(0xff, 0xd1, 0x66),
    RGB565(0x66, 0xe0, 0xd2), RGB565(0x8b, 0x5c, 0xf6),
};
static const char *LED_COLOR_NAMES[4] = {"COLOR", "WHITE", "RED", "GREEN"};

/* ---------- helpers ---------- */
static void title_c(const char *s, int y) { gfx_text_center(CW / 2, y, s, PAL.white, 1); }
static void hint_c(const char *s, int y) { gfx_text_center(CW / 2, y, s, PAL.dim, 1); }
static void hint_l(const char *s, int x, int y) { gfx_text(x, y, s, PAL.dim, 1); }

static void panel_box(int x, int y, int w, int h, bool sel, bool cur) {
    uint16_t border = sel ? PAL.cyan : (cur ? PAL.white : PAL.line);
    uint16_t fill = sel ? PAL.seltint : PAL.panel;
    gfx_pxbox(x, y, w, h, border, 1, fill);
}

static void draw_multiline(int cx, int y, const char *s, uint16_t c, int scale, int lineH, bool center) {
    char line[80];
    int n = 0;
    for (const char *p = s;; p++) {
        if (*p == '\n' || *p == 0) {
            line[n] = 0;
            if (center) gfx_text_center(cx, y, line, c, scale);
            else gfx_text(cx, y, line, c, scale);
            y += lineH; n = 0;
            if (*p == 0) break;
        } else if (n < 79) line[n++] = *p;
    }
}

static int nav_dir(const event_t *ev) {
    if (ev->type != EV_PRESS && ev->type != EV_LONG && ev->type != EV_REPEAT) return 0;
    if (ev->button == BTN_UP) return -1;
    if (ev->button == BTN_DOWN) return 1;
    return 0;
}

static void windowed(int len, int sel, int K, int *start, int *end) {
    if (len <= K) { *start = 0; *end = len; return; }
    int st = sel - K / 2;
    if (st < 0) st = 0;
    if (st > len - K) st = len - K;
    *start = st; *end = st + K;
}

static const char *fix_str(int fix) {
    return fix == 2 ? "3D FIX" : (fix == 1 ? "2D FIX" : "NO FIX");
}

/* ================= HOME ================= */
static void render_home(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text_center(CW / 2, 40, "RACE", PAL.white, 3);
    gfx_text_center(CW / 2, 68, "CHRONO", PAL.cyan, 2);
    int bw = 190, bh = 34, bx = (CW - bw) / 2, by = 100;
    gfx_pxbox(bx, by, bw, bh, PAL.yellow, 0, 0);
    if (((int)(g.now / 500)) % 2 == 0)
        gfx_text_center(CW / 2, by + (bh - 14) / 2, "START RACING", PAL.yellow, 2);
    for (int i = 0; i < 4; i++) {
        int dx = CW / 2 - 3 * 6 - 3 + i * 12;
        gfx_fill(dx, 150, 6, 6, i == 0 ? PAL.cyan : PAL.dotoff);
    }
    hint_c("UP/DOWN  SWITCH PAGE", 166); /* kept */
    hint_c("PUSH  START", 180);
}

/* ================= TRACK EDIT ================= */
static void render_track_edit(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("TRACK EDIT", 6);
    hint_c("EDIT ON PHONE APP", 20);
    int y = 36;
    for (int i = 0; i < g.ntracks && i < 3; i++) {
        bool on = g.entered && i == g.sel;
        panel_box(12, y, CW - 24, 44, on, false);
        gfx_text(22, y + 8, g.tracks[i].name, PAL.white, 1);
        gfx_text(22, y + 24, VEH_NAMES[g.tracks[i].vehicle], PAL.dim, 1);
        y += 48;
    }
    if (!g.ntracks) hint_c("(NO TRACKS)", 60);
    hint_c(g.entered ? "PUSH VIEW - BACK EXIT" : "PUSH EDIT - UP/DOWN PAGE", 182);
}

/* ================= SETTINGS ================= */
static const char *settings_key(int i) {
    static const char *k[12] = {"BRIGHTNESS", "LED COLOR", "LED BRIGHT", "THEME", "WIFI", "BLUETOOTH", "SOUND", "GPS RATE", "IMU DEBUG", "GPS TRACK", "GPS DEBUG", "LOCATION"};
    return k[i];
}
static void settings_val(int i, char *out) {
    switch (i) {
    case 0: snprintf(out, 16, "%d%%", g.set_brightness); break;
    case 1: strcpy(out, LED_COLOR_NAMES[g.set_ledcolor]); break;
    case 2: snprintf(out, 16, "%d%%", g.set_ledbright); break;
    case 3: strcpy(out, g.theme_light ? "LIGHT" : "DARK"); break;
    case 4: strcpy(out, g.set_wifi ? "ON" : "OFF"); break;
    case 5: strcpy(out, g.set_bt ? "ON" : "OFF"); break;
    case 7: snprintf(out, 16, "%dHz", g.gps_rate_hz); break;
    default: strcpy(out, ">"); break;
    }
}
static void render_settings(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SETTINGS", 2);
    const int K = 5, h = 32;
    int start, end;
    windowed(12, g.sel, K, &start, &end);
    int y = 20;
    for (int i = start; i < end; i++) {
        bool on = g.entered && i == g.sel;
        panel_box(10, y, CW - 20, h - 4, on, false);
        gfx_text(20, y + 7, settings_key(i), PAL.text, 2);
        char v[16]; settings_val(i, v);
        gfx_text_right(CW - 18, y + 7, v, PAL.cyan, 2);
        y += h;
    }
    /* scroll indicators */
    if (start > 0) gfx_text(CW - 10, 4, "^", PAL.dim, 1);
    if (end < 12) gfx_text(CW - 10, 20 + K * h - 14, "v", PAL.dim, 1);
}

/* ================= SOUND ================= */
static void render_sound(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SOUND", 2);
    static const char *keys[4] = {"VOLUME", "CONFIRM", "CALIB", "3D FIX"};
    const int h = 32;
    int y = 24;
    for (int i = 0; i < 4; i++) {
        bool on = i == g.sel;
        panel_box(10, y, CW - 20, h - 4, on, false);
        gfx_text(20, y + 7, keys[i], PAL.text, 2);
        char v[16];
        if (i == 0) snprintf(v, sizeof v, "%d%%", g.set_sound_vol);
        else {
            bool b = i == 1 ? g.set_snd_confirm : (i == 2 ? g.set_snd_calib : g.set_snd_fix);
            strcpy(v, b ? "ON" : "OFF");
        }
        gfx_text_right(CW - 18, y + 7, v, PAL.cyan, 2);
        y += h;
    }
    hint_c("UP/DOWN SELECT - PUSH ADJUST - BACK", 182);
}

/* ================= IMU DEBUG (real MPU6050) ================= */
static void dbg_bar(int y, const char *label, float v, float half, uint16_t col) {
    gfx_text(150, y, label, PAL.dim, 0);
    int x0 = 176, w = 96, xc = x0 + w / 2;
    gfx_fill(x0, y + 1, w, 6, PAL.surface2);
    gfx_vline(xc, y - 1, 9, PAL.centerline);
    float f = v / half; if (f > 1) f = 1; if (f < -1) f = -1;
    int len = (int)(f * (w / 2));
    if (len >= 0) gfx_fill(xc, y + 2, len, 4, col);
    else          gfx_fill(xc + len, y + 2, -len, 4, col);
    char b[12]; u_fmt_f(b, sizeof b, v, 2);
    gfx_text(280, y, b, PAL.text, 0);
}

static void render_imu_debug(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(6, 2, "IMU DEBUG", PAL.cyan, 1);
    gfx_text(6, 18, g.imu_ok ? "MPU6050 OK" : "MPU6050 NOT FOUND",
             g.imu_ok ? PAL.green : PAL.red, 1);

    /* bubble level (tilt from accel X/Y) */
    int cx = 76, cy = 112, R = 58;
    gfx_circle(cx, cy, R, PAL.centerline);
    gfx_circle(cx, cy, R / 2, PAL.centerline);
    gfx_hline(cx - R, cy, 2 * R, PAL.centerline);
    gfx_vline(cx, cy - R, 2 * R, PAL.centerline);
    gfx_circle(cx, cy, 4, PAL.centerline);
    if (g.imu_ok) {
        float a3[3] = {g.i_ax, g.i_ay, g.i_az};
        float fwd = a3[IMU_BUB_FWD_AXIS] * IMU_BUB_FWD_SIGN;   /* + = forward */
        float lat = a3[IMU_BUB_LAT_AXIS] * IMU_BUB_LAT_SIGN;   /* + = left */
        if (fwd > 1) fwd = 1; if (fwd < -1) fwd = -1;
        if (lat > 1) lat = 1; if (lat < -1) lat = -1;
        int px = cx - (int)(lat * (R - 8));   /* left -> left */
        int py = cy - (int)(fwd * (R - 8));   /* forward -> up */
        gfx_disc(px, py, 7, PAL.cyan);
        gfx_disc(px, py, 3, PAL.red);
    }
    gfx_text(cx - 12, cy + R + 5, "TILT", PAL.dim, 0);

    /* G force, calibrated to the boot pose (0 at rest) */
    gfx_text(150, 34, "G FORCE", PAL.cyan, 0);
    dbg_bar(46, "GX", g.i_ax, 2.0f, PAL.green);
    dbg_bar(62, "GY", g.i_ay, 2.0f, PAL.green);
    dbg_bar(78, "GZ", g.i_az, 2.0f, PAL.cyan);
    /* rotation rate */
    gfx_text(150, 94, "RATE d/s", PAL.cyan, 0);
    dbg_bar(106, "RX", g.i_gx, 250.0f, PAL.yellow);
    dbg_bar(122, "RY", g.i_gy, 250.0f, PAL.yellow);
    dbg_bar(138, "RZ", g.i_gz, 250.0f, PAL.orange);

    char t[16];
    {
        float a3[3] = {g.i_ax, g.i_ay, g.i_az};
        float h1 = a3[(g.i_vaxis + 1) % 3], h2 = a3[(g.i_vaxis + 2) % 3];
        u_fmt_f(t, sizeof t, sqrtf(h1 * h1 + h2 * h2), 2);
    }
    gfx_text(150, 158, "|G|", PAL.dim, 0);
    gfx_text(172, 158, t, PAL.white, 0);
    u_fmt_f(t, sizeof t, g.i_gmax, 2);
    gfx_text(228, 158, "PEAK", PAL.dim, 0);
    gfx_text(256, 158, t, PAL.gold, 0);

    u_fmt_f(t, sizeof t, g.i_temp, 1);
    gfx_text(150, 174, "TEMP", PAL.dim, 0);
    gfx_text(180, 174, t, PAL.text, 0);
    gfx_text(214, 174, "C", PAL.dim, 0);

    gfx_text(6, 186, "PUSH TARE - BACK EXIT", PAL.dim, 0);
}

/* ================= GPS TRACK (real driven path) ================= */
static void render_gps_track(void) {
    gps_state_t *gp = gps_get();
    rt_path_t *rt = rt_get();
    char b[48];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(6, 2, "GPS TRACK", PAL.cyan, 1);
    snprintf(b, sizeof b, "FIX %s  SAT %d  PTS %d",
             gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : "NO"), gp->sats_used, rt->n);
    gfx_text(6, 18, b, gp->fix_type >= 1 ? PAL.green : PAL.orange, 1);

    int mx = 4, my = 34, mw = CW - 8, mh = 128;
    gfx_fill(mx, my, mw, mh, PAL.mapbg);
    gfx_rect(mx, my, mw, mh, PAL.mapborder);

    if (rt->n > 1) {
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (int i = 0; i < rt->n; i++) {
            pt_t q = rt->pts[i];
            if (q.x < minx) minx = q.x;
            if (q.x > maxx) maxx = q.x;
            if (q.y < miny) miny = q.y;
            if (q.y > maxy) maxy = q.y;
        }
        float spx = maxx - minx, spy = maxy - miny;
        if (spx < 3) spx = 3;
        if (spy < 3) spy = 3;
        float s = (mw - 10) / spx, sy = (mh - 10) / spy;
        if (sy < s) s = sy;
        float ox = mx + mw / 2.0f - (minx + maxx) / 2 * s;
        float oy = my + mh / 2.0f - (miny + maxy) / 2 * s;
        for (int i = 0; i < rt->n; i++) {
            int px = (int)lroundf(rt->pts[i].x * s + ox);
            int py = (int)lroundf(rt->pts[i].y * s + oy);
            gfx_fill(px - 1, py - 1, 2, 2, PAL.gpstrace);
        }
        int px = (int)lroundf(rt->pts[rt->n - 1].x * s + ox);
        int py = (int)lroundf(rt->pts[rt->n - 1].y * s + oy);
        gfx_fill(px - 2, py - 2, 5, 5, PAL.red);
        gfx_fill(px - 1, py - 1, 3, 3, PAL.white);
    } else {
        gfx_text(mx + 8, my + 8, gp->fix_type >= 1 ? "move to draw..." : "waiting for fix...", PAL.dim, 0);
    }

    snprintf(b, sizeof b, "PATH %.1f M   SPD %.0f KM/H", rt->dist, gp->speed_kmh);
    gfx_text(6, 168, b, PAL.white, 1);
    gfx_text(6, 184, "PUSH CLEAR - BACK EXIT", PAL.dim, 0);
}

/* ================= MODE SELECT ================= */
static void mode_box(int y, const char *name, const char *sub, bool sel) {
    panel_box(30, y, CW - 60, 56, sel, false);
    gfx_text_center(CW / 2, y + 14, name, sel ? PAL.cyan : PAL.white, 1);
    gfx_text_center(CW / 2, y + 34, sub, sel ? PAL.text2 : PAL.dim, 1);
}
static void render_mode_select(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SELECT MODE", 6);
    mode_box(40, "NEW TRACK", "MAP + RACE MODE", g.sel == 0);
    mode_box(112, "RECORDED TRACK", "KNOWN TRACK", g.sel == 1);
    hint_c("UP/DOWN SELECT - PUSH ENTER - BACK", 182);
}

/* ================= MOUNT SELECT ================= */
static void render_mount_select(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("MOUNTING", 6);
    mode_box(40, "FIXED (CHASSIS)", "RIGID - TRUST IMU", g.sel == 0);
    mode_box(112, "NOT FIXED (WHEEL)", "ROTATES - TRUST GPS", g.sel == 1);
    hint_c("UP/DOWN SELECT - PUSH START - BACK", 182);
}

/* ================= NEW TRACK - MARK START ================= */
static void render_record_track(void) {
    gps_state_t *gp = gps_get();
    char b[32];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(6, 2, "NEW TRACK", PAL.cyan, 1);
    const char *fx = gp->fix_type == 2 ? "3D FIX" : (gp->fix_type == 1 ? "2D FIX" : "NO FIX");
    gfx_text(6, 18, fx, gp->fix_type == 2 ? PAL.green : PAL.orange, 1);
    snprintf(b, sizeof b, "SAT %d", gp->sats_used);
    gfx_text(72, 18, b, PAL.dim, 1);
    if (g.rec_state == 1) {
        bool blink = ((int)(g.now / 400)) % 2 == 0;
        gfx_text(180, 18, "REC", blink ? PAL.red : PAL.dim, 1);
    }

    /* live recorded path */
    int mx = 4, my = 36, mw = 184, mh = 126;
    gfx_fill(mx, my, mw, mh, PAL.mapbg);
    gfx_rect(mx, my, mw, mh, PAL.mapborder);
    if (g.rec_n > 1) {
        float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
        for (int i = 0; i < g.rec_n; i++) {
            pt_t q = g.rec_pts[i];
            if (q.x < minx) minx = q.x;
            if (q.x > maxx) maxx = q.x;
            if (q.y < miny) miny = q.y;
            if (q.y > maxy) maxy = q.y;
        }
        float spx = maxx - minx, spy = maxy - miny;
        if (spx < 4) spx = 4;
        if (spy < 4) spy = 4;
        float s = (mw - 10) / spx, sy = (mh - 10) / spy;
        if (sy < s) s = sy;
        float ox = mx + mw / 2.0f - (minx + maxx) / 2 * s;
        float oy = my + mh / 2.0f - (miny + maxy) / 2 * s;
        for (int i = 0; i < g.rec_n; i++) {
            int px = (int)lroundf(g.rec_pts[i].x * s + ox);
            int py = (int)lroundf(g.rec_pts[i].y * s + oy);
            gfx_fill(px - 1, py - 1, 2, 2, PAL.gpstrace);
        }
        int px = (int)lroundf(g.rec_last.x * s + ox);
        int py = (int)lroundf(g.rec_last.y * s + oy);
        gfx_fill(px - 2, py - 2, 5, 5, PAL.red);
        gfx_fill(px - 1, py - 1, 3, 3, PAL.white);
    } else {
        gfx_text(mx + 8, my + 8, g.rec_state == 1 ? "recording..." : "at the start line", PAL.dim, 0);
    }

    /* stats + hint */
    int rx = 196;
    snprintf(b, sizeof b, "%.0f M", g.rec_dist);
    gfx_text(rx, 40, "DIST", PAL.dim, 0);
    gfx_text(rx, 50, b, PAL.white, 1);
    snprintf(b, sizeof b, "%d", g.rec_n);
    gfx_text(rx, 74, "PTS", PAL.dim, 0);
    gfx_text(rx, 84, b, PAL.white, 1);

    const char *hint;
    uint16_t hc;
    if (g.rec_state == 1) { hint = "PUSH FINISH"; hc = PAL.yellow; }
    else if (gp->fix_type >= 2) { hint = "PUSH START"; hc = PAL.cyan; }
    else { hint = "WAIT 3D FIX"; hc = PAL.orange; }
    gfx_text(rx, 112, hint, hc, 1);
    if (gp->fix_type < 2)
        gfx_text(6, 170, "DOWN: GPS DEBUG   BACK CANCEL", PAL.orange, 0);
    else
        gfx_text(6, 170, "PUSH START/FINISH   BACK CANCEL", PAL.dim, 0);
}

/* ================= RACING ================= */
static void render_panel_dots(void) {
    for (int i = 0; i < 4; i++) {
        int x = CW - 6 - (4 - i) * 9;
        gfx_fill(x, CH - 10, 5, 5, i == g.panel ? PAL.cyan : PAL.dotoff);
    }
}

static void racing_map(bool recorded, engine_t *e, float dt) {
    if (!s_map_ready) {
        map_init(&s_map, 0, 0, 150, 200);
        if (recorded && e->track) map_set_track(&s_map, e->track);
        s_map_ready = true;
    }
    map_begin(&s_map);
    if (recorded) {
        map_centerline(&s_map, PAL.centerline, 2);
    } else {
        map_fit_engine(&s_map, e, 0.28f);
        map_smooth(&s_map, dt, 4);
    }
    for (int l = 0; l < e->nLaps; l++)
        map_trace(&s_map, e->laps[l].gps, e->laps[l].nGps, PAL.gpstracehist, 2);
    map_trace(&s_map, e->gpsLapTrace, e->nGps, PAL.gpstrace, 2);
    map_trail(&s_map, e);
    if (recorded) {
        map_start_line(&s_map, 0, PAL.white);
        for (int i = 0; i < e->track->nsec; i++) map_tick(&s_map, e->track->sections[i], PAL.orange, 4);
        map_car(&s_map, engine_progress(e), PAL.green);
    } else {
        pt_t sp = track_point_at(e->track, 0);
        pt_t st = track_tangent_at(e->track, 0);
        map_start_mark_at(&s_map, sp, st, PAL.white);
        map_car_at(&s_map, track_point_at(e->track, engine_progress(e)),
                   track_tangent_at(e->track, engine_progress(e)), PAL.green);
    }
}

static void render_racing_panel(bool recorded, engine_t *e, float dt) {
    char b[24], b2[24];
    if (g.panel == 0) {
        racing_map(recorded, e, dt);
        int rx = 156;
        gfx_text(rx, 6, "LIVE", PAL.dim, 1);
        u_fmt_time(b, sizeof b, e->elapsed);
        gfx_text(rx, 20, b, PAL.white, 2);
        u_fmt_delta(b2, sizeof b2, e->diff, 3);
        uint16_t dc = fabsf(e->diff) < 0.02f ? PAL.dim : (e->diff > 0 ? PAL.red : PAL.green);
        gfx_text(rx, 44, b2, dc, 2);
        int shown = e->nLaps < 3 ? e->nLaps : 3;
        for (int k = 0; k < shown; k++) {
            int idx = e->nLaps - 1 - k;
            lap_t *lap = &e->laps[idx];
            int y = 72 + k * 22;
            bool best = e->hasBestLap && lap->index == e->bestIndex;
            if (best) gfx_fill(rx, y, CW - rx - 6, 20, PAL.purple);
            else gfx_fill(rx, y, CW - rx - 6, 20, PAL.surface);
            snprintf(b2, sizeof b2, "L%d", lap->index);
            gfx_text(rx + 6, y + 6, b2, best ? PAL.white : PAL.dim, 1);
            u_fmt_time(b, sizeof b, lap->time);
            gfx_text_right(CW - 12, y + 6, b, best ? PAL.white : PAL.text2, 1);
        }
        if (shown == 0) gfx_text(rx + 6, 78, "--", PAL.dim, 1);
        if (recorded && e->track->hasBest) {
            int y = 72 + shown * 22 + 4;
            gfx_fill(rx, y, CW - rx - 6, 20, PAL.goldtint);
            gfx_text(rx + 6, y + 6, "H-BEST", PAL.gold, 1);
            u_fmt_time(b, sizeof b, e->track->best);
            gfx_text_right(CW - 12, y + 6, b, PAL.gold, 1);
        }
    } else if (g.panel == 1) {
        gfx_text(12, 8, "LAP", PAL.cyan, 1);
        snprintf(b2, sizeof b2, "%d", e->lap);
        gfx_text(12 + gfx_text_w("LAP ", 1), 8, b2, PAL.cyan, 1);
        u_fmt_time(b, sizeof b, e->hasBestLap ? e->bestTime : NAN);
        char bb[32]; snprintf(bb, sizeof bb, "BEST %s", b);
        gfx_text_right(CW - 12, 8, bb, PAL.gold, 1);
        u_fmt_time(b, sizeof b, e->elapsed);
        gfx_text_center(CW / 2, 60, b, PAL.white, 5);
        u_fmt_delta(b2, sizeof b2, e->diff, 3);
        uint16_t dc = fabsf(e->diff) < 0.02f ? PAL.dim : (e->diff > 0 ? PAL.red : PAL.green);
        gfx_text_center(CW / 2, 108, b2, dc, 3);
        if (e->nLaps) {
            u_fmt_time(b, sizeof b, e->laps[e->nLaps - 1].time);
            snprintf(bb, sizeof bb, "LAST %s", b);
        } else snprintf(bb, sizeof bb, "LAST --:--.---");
        gfx_text_center(CW / 2, 150, bb, PAL.dim, 1);
        hint_c("UP/DOWN SWITCH PANEL", 178);
    } else if (g.panel == 2) {
        int sx = (int)lroundf(engine_speed_kmh(e));
        snprintf(b, sizeof b, "%d", sx);
        gfx_text(14, 44, b, PAL.white, 5);
        gfx_text(16, 92, "KM/H", PAL.dim, 1);
        float sp = engine_speed_kmh(e) / e->track->maxSpeed * 100.0f;
        if (sp > 100) sp = 100;
        gfx_rect(14, 108, 100, 8, PAL.line);
        gfx_fill(15, 109, (int)(98 * sp / 100.0f), 6, PAL.cyan);
        char dd[24]; snprintf(dd, sizeof dd, "%d M", (int)lroundf(engine_dist_m(e)));
        gfx_text(14, 122, dd, PAL.cyan, 1);
        map_draw_gmeter(120, 30, 108, 108, e->gLat, e->gLong, e->gTrail, e->nGTrail, 2);
        char gv[32]; gv[0] = 'G'; gv[1] = ' ';
        u_fmt_f(gv + 2, (int)sizeof(gv) - 2, hypotf(e->gLat, e->gLong), 2);
        gfx_text_center(174, 142, gv, PAL.text, 1);
        int rx = 238;
        const char *keys[6] = {"SATS", "HDOP", "FIX", "KF-S", "HDG", "ALT"};
        char vals[6][16];
        snprintf(vals[0], 16, "%d/%d", g.gnss.used, g.gnss.visible);
        u_fmt_f(vals[1], 16, g.gnss.hdop, 1);
        snprintf(vals[2], 16, "%s", fix_str(g.gnss.fix));
        u_fmt_f(vals[3], 16, e->fusion.sigma, 4);
        snprintf(vals[4], 16, "%d", (int)lroundf(engine_heading_deg(e)));
        u_fmt_f(vals[5], 16, engine_alt_m(e), 1);
        for (int i = 0; i < 6; i++) {
            int y = 24 + i * 26;
            gfx_text(rx, y, keys[i], PAL.dim, 0);
            uint16_t c = PAL.white;
            if (i == 2) c = g.gnss.fix ? PAL.green : PAL.red;
            if (i == 3) c = e->gpsFix ? PAL.green : PAL.red;
            gfx_text_right(CW - 12, y + 10, vals[i], c, 0);
            gfx_hline(rx, y + 22, CW - 12 - rx, PAL.line2);
        }
    } else {
        gfx_fill(0, 0, CW, CH, PAL.stage);
        title_c("SECTOR SPLITS", 6);
        int n = recorded ? e->track->nsec : g.session_nsec;
        const float *sections = recorded ? e->track->sections : g.sessionSections;
        float deltas[MAX_SECTIONS];
        engine_section_deltas(e, deltas);
        int cur = -1;
        float prog = engine_progress(e);
        for (int i = 0; i < n; i++) if (prog <= sections[i]) { cur = i; break; }
        if (cur < 0) cur = n - 1;
        if (!n) {
            hint_c("NO SECTION DATA", 80);
        } else {
            float start = 0;
            for (int i = 0; i < n && i < 6; i++) {
                int y = 26 + i * 26;
                bool on = i == cur;
                gfx_pxbox(12, y, CW - 24, 24, on ? PAL.cyan : PAL.line, 1, on ? PAL.seltint : PAL.surface);
                snprintf(b, sizeof b, "S%d", i + 1);
                gfx_text(20, y + 8, b, PAL.white, 1);
                snprintf(b, sizeof b, "%d-%d%%", (int)(start * 100), (int)(sections[i] * 100));
                gfx_text(70, y + 8, b, PAL.dim, 1);
                if (isnan(deltas[i])) {
                    gfx_text_right(CW - 20, y + 8, "--.---", PAL.dim, 1);
                } else {
                    u_fmt_delta(b2, sizeof b2, deltas[i], 3);
                    uint16_t c = fabsf(deltas[i]) < 0.02f ? PAL.dim : (deltas[i] > 0 ? PAL.red : PAL.green);
                    gfx_text_right(CW - 20, y + 8, b2, c, 1);
                }
                start = sections[i];
            }
        }
        hint_c("PUSH END RACE - UP/DOWN PANEL", 182);
    }
    render_panel_dots();
}

/* ================= LAP SELECT ================= */
static void render_lap_select(void) {
    char b[24];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    if (!s_map_ready) { map_init(&s_map, 0, 0, 176, 200); if (g.engine.track) map_set_track(&s_map, g.engine.track); s_map_ready = true; }
    map_begin(&s_map);
    map_centerline(&s_map, PAL.centerline, 2);
    for (int i = 0; i < g.engine.nLaps; i++)
        map_trace(&s_map, g.engine.laps[i].trace, g.engine.laps[i].nTrace,
                  g.lapChecked[i] ? PAL.cyan : PAL.histtrace, 2);
    title_c("SELECT LAPS", 6);
    int start, end;
    windowed(g.engine.nLaps, g.sel, 5, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 24 + (i - start) * 24;
        bool cur = i == g.sel;
        gfx_pxbox(182, y, 132, 22, cur ? PAL.white : PAL.line, 1, PAL.surface);
        gfx_fill(190, y + 7, 8, 8, g.lapChecked[i] ? PAL.green : PAL.checkboxoff);
        snprintf(b, sizeof b, "L%d", g.engine.laps[i].index);
        gfx_text(206, y + 8, b, PAL.dim, 1);
        u_fmt_time(b, sizeof b, g.engine.laps[i].time);
        gfx_text_right(310, y + 8, b, PAL.text2, 1);
    }
    int nsel = 0;
    for (int i = 0; i < g.engine.nLaps; i++) if (g.lapChecked[i]) nsel++;
    snprintf(b, sizeof b, "PICKED %d / MIN 2", nsel);
    gfx_text(182, 158, b, PAL.dim, 0);
    gfx_text(182, 174, "PUSH CHECK - 2xPUSH GO", PAL.dim, 0);
}

/* ================= SECTION MARKING ================= */
static void render_section(void) {
    char b[32];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    if (!s_map_ready) { map_init(&s_map, 0, 0, 176, 200); map_set_track(&s_map, &g.sessionTrack); s_map_ready = true; }
    map_begin(&s_map);
    map_centerline(&s_map, PAL.centerline, 2);
    map_start_line(&s_map, 0, PAL.white);
    for (int i = 0; i < g.sec_n; i++) map_tick(&s_map, g.sec_sections[i], PAL.orange, 6);
    if (!isnan(g.sec_cand)) map_tick(&s_map, g.sec_cand, PAL.yellow, 7);
    bool blink = ((int)(g.now / 250)) % 2 == 0;
    map_marker(&s_map, g.sec_pos, PAL.orange, 5, blink);
    title_c("SECTION", 6);
    snprintf(b, sizeof b, "MARKED %d", g.sec_n);
    gfx_text(182, 24, b, PAL.orange, 1);
    snprintf(b, sizeof b, "POS %d%%", (int)(g.sec_pos * 100));
    gfx_text(182, 40, b, PAL.orange, 1);
    snprintf(b, sizeof b, "REMAIN %d%%", (int)((1 - g.sec_prev) * 100));
    gfx_text(182, 56, b, PAL.orange, 1);
    if (!isnan(g.sec_cand)) {
        snprintf(b, sizeof b, "CAND %d%%", (int)(g.sec_cand * 100));
        gfx_text(182, 72, b, PAL.yellow, 1);
    }
    gfx_text(182, 120, "UP/DOWN MOVE", PAL.dim, 1);
    gfx_text(182, 134, "PUSH MARK", PAL.dim, 1);
    gfx_text(182, 148, "2xPUSH FINISH", PAL.dim, 1);
    gfx_text(182, 162, "BACK EXIT", PAL.dim, 1);
}

/* ================= VEHICLE SELECT ================= */
static void render_vehicle(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SELECT VEHICLE", 6);
    for (int i = 0; i < 4; i++) {
        int cx = 160 + (i - g.sel) * 120;
        if (cx < -60 || cx > CW + 60) continue;
        bool on = i == g.sel;
        map_draw_vehicle(cx - 36, 46, 72, 56, on ? VEH_COLORS[i] : PAL.glyphoff, PAL.yellow);
        gfx_text_center(cx, 112, VEH_NAMES[i], on ? PAL.cyan : PAL.dim, 1);
    }
    hint_c("UP/DOWN SELECT - PUSH CONFIRM", 182);
}

/* ================= TRACK NAMING ================= */
static void render_naming(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("NAME TRACK", 6);
    char big[2] = {g.avail[g.sel], 0};
    gfx_text_center(CW / 2, 36, big, PAL.cyan, 8);
    int step = 8, total = g.navail * step;
    int x0 = (CW - total) / 2;
    for (int i = 0; i < g.navail; i++) {
        char ch[2] = {g.avail[i], 0};
        if (i == g.sel) {
            gfx_fill(x0 + i * step - 1, 136, 8, 14, PAL.cyan);
            gfx_text(x0 + i * step, 140, ch, PAL.white, 0);
        } else {
            gfx_text(x0 + i * step, 140, ch, PAL.glyphoff, 0);
        }
    }
    hint_c("UP/DOWN LETTER - PUSH SAVE", 182);
}

/* ================= TRACK LIST ================= */
static void render_track_list(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("TRACKS", 6);
    if (!g.ntracks) hint_c("(NO TRACKS)", 60);
    int start, end;
    windowed(g.ntracks, g.cursor, 3, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 24 + (i - start) * 50;
        bool cur = i == g.cursor;
        gfx_pxbox(12, y, CW - 24, 46, cur ? PAL.white : PAL.line, 1, PAL.panel);
        map_draw_thumb(20, y + 8, 40, 30, &g.tracks[i], PAL.dim);
        gfx_text(70, y + 8, g.tracks[i].name, PAL.white, 1);
        gfx_text(70, y + 24, VEH_NAMES[g.tracks[i].vehicle], PAL.dim, 1);
    }
    hint_c("UP/DOWN SELECT - PUSH ENTER", 182);
}

/* ================= HISTORY ================= */
static void render_history(void) {
    char b[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    map_init(&s_map, 0, 0, 176, 200);
    map_begin(&s_map);
    if (g.nlog) {
        histrec_t *rec = &g.log[g.nlog - 1 - g.sel];
        map_fit_points(&s_map, rec->trace, rec->nTrace, 0.3f);
        map_trace(&s_map, rec->trace, rec->nTrace, PAL.purple, 2);
    }
    title_c("RACE LOG", 6);
    int start, end;
    windowed(g.nlog, g.sel, 3, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 24 + (i - start) * 40;
        bool on = g.entered && i == g.sel;
        gfx_pxbox(182, y, 132, 36, on ? PAL.white : PAL.line, 1, PAL.panel);
        histrec_t *r = &g.log[g.nlog - 1 - i];
        snprintf(b, sizeof b, "%s  %dL", r->trackName, r->lapCount);
        gfx_text(188, y + 6, b, PAL.white, 1);
        u_fmt_time(b, sizeof b, r->duration);
        gfx_text(188, y + 20, r->date, PAL.dim, 0);
        gfx_text_right(310, y + 20, b, PAL.dim, 0);
    }
    if (!g.nlog) hint_c("(NO RECORDS)", 60);
    gfx_text(182, 158, g.entered ? "PUSH DETAIL" : "PUSH ENTER", PAL.dim, 1);
    gfx_text(182, 174, "BACK EXIT", PAL.dim, 1);
}

/* ================= HISTORY DETAIL ================= */
static void render_history_detail(void) {
    char b[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    histrec_t *r = (g.hist_rec >= 0 && g.hist_rec < g.nlog) ? &g.log[g.hist_rec] : NULL;
    map_init(&s_map, 0, 0, 176, 200);
    map_begin(&s_map);
    if (r) {
        int sel = g.sel < r->lapCount ? g.sel : 0;
        (void)sel;
        map_fit_points(&s_map, r->trace, r->nTrace, 0.3f);
        map_trace(&s_map, r->trace, r->nTrace, PAL.cyan, 2);
        for (int i = 0; i < r->nsec; i++) {
            int idx = (int)(r->sections[i] * (r->nTrace - 1));
            if (idx >= 0 && idx < r->nTrace) map_marker_at(&s_map, r->trace[idx], PAL.orange, 4, true);
        }
    }
    if (!r) { title_c("RACE", 6); hint_c("NO RECORD", 60); return; }
    snprintf(b, sizeof b, "RACE %s", r->trackName);
    title_c(b, 6);
    u_fmt_time(b, sizeof b, r->duration);
    char sb[48];
    snprintf(sb, sizeof sb, "%dL - %s", r->lapCount, b);
    gfx_text(182, 22, sb, PAL.cyan, 1);
    int start, end;
    windowed(r->lapCount, g.sel, 4, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 38 + (i - start) * 20;
        bool best = r->hasBest && r->lapIndex[i] == r->bestIndex;
        bool cur = i == g.sel;
        if (best) gfx_fill(182, y, 132, 18, PAL.purple);
        else gfx_fill(182, y, 132, 18, PAL.surface);
        if (cur) gfx_rect(182, y, 132, 18, PAL.white);
        snprintf(b, sizeof b, "L%d", r->lapIndex[i]);
        gfx_text(188, y + 5, b, best ? PAL.white : PAL.dim, 1);
        u_fmt_time(b, sizeof b, r->lapTime[i]);
        gfx_text_right(310, y + 5, b, best ? PAL.white : PAL.text2, 1);
    }
    gfx_text(182, 122, "SECTION BEST", PAL.gold, 1);
    for (int i = 0; i < r->nsec && i < 3; i++) {
        int y = 136 + i * 16;
        snprintf(b, sizeof b, "S%d", i + 1);
        gfx_text(182, y, b, PAL.white, 1);
        if (r->secValid[i]) {
            u_fmt_time(b, sizeof b, r->secBest[i]);
            gfx_text(208, y, b, PAL.green, 1);
            snprintf(b, sizeof b, "L%d", r->secBestLap[i]);
            gfx_text_right(310, y, b, PAL.gold, 1);
        } else {
            gfx_text(208, y, "--", PAL.dim, 1);
        }
    }
}

/* ================= GNSS TEST ================= */
/* ================= SAT VIEW (real NEO-M9N sats, debug sub-page 2) ================= */
static void render_sat_page(void) {
    gps_state_t *gp = gps_get();
    char b[48];
    gfx_fill(0, 0, CW, CH, PAL.stage);

    /* satellites seen with a signal, strongest SNR first */
    int order[GPS_NSAT], no = 0;
    for (int i = 0; i < gp->nsat; i++)
        if (gp->sats[i].seen && gp->sats[i].snr > 0) order[no++] = i;
    for (int i = 0; i < no; i++)
        for (int j = i + 1; j < no; j++)
            if (gp->sats[order[j]].snr > gp->sats[order[i]].snr) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }

    snprintf(b, sizeof b, "SATELLITES  %d VIS  %d USED", no, gp->sats_used);
    gfx_text(6, 2, b, PAL.cyan, 1);
    snprintf(b, sizeof b, "%s  HDOP %.1f  %s", gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : "NO"),
             gp->hdop, gp->source == 1 ? "I2C" : "UART");
    gfx_text(6, 18, b, gp->fix_type >= 1 ? PAL.green : PAL.orange, 1);

    /* column header */
    gfx_text(6, 38, "SYS", PAL.dim, 0);
    gfx_text(42, 38, "SNR", PAL.dim, 0);
    gfx_text(206, 38, "EL", PAL.dim, 0);
    gfx_text(240, 38, "AZ", PAL.dim, 0);
    gfx_text(290, 38, "U", PAL.dim, 0);

    for (int r = 0; r < 8 && r < no; r++) {
        gps_sat_t *s = &gp->sats[order[r]];
        int y = 50 + r * 19;
        const char *sn = s->sys == 'P' ? "G" : s->sys == 'B' ? "B" : s->sys == 'A' ? "E" :
                         s->sys == 'L' ? "R" : s->sys == 'Q' ? "J" : "?";
        uint16_t col = s->snr >= 40 ? PAL.green : (s->snr >= 30 ? PAL.yellow : PAL.red);
        snprintf(b, sizeof b, "%s%02d", sn, s->prn);
        gfx_text(6, y, b, s->used ? PAL.cyan : PAL.text, 1);
        snprintf(b, sizeof b, "%d", s->snr);
        gfx_text(42, y, b, col, 1);
        int bx = 78, bw = 116;
        gfx_fill(bx, y + 3, bw, 6, PAL.surface2);
        int w = (int)(bw * (s->snr / 50.0f));
        if (w > bw) w = bw;
        gfx_fill(bx, y + 3, w, 6, col);
        snprintf(b, sizeof b, "%d", s->el);
        gfx_text(206, y, b, PAL.text2, 1);
        snprintf(b, sizeof b, "%d", s->az);
        gfx_text(240, y, b, PAL.text2, 1);
        if (s->used) gfx_text(290, y, "*", PAL.green, 1);
    }
    if (no == 0) gfx_text(6, 50, "no satellites", PAL.dim, 1);
}

/* ================= GPS DEBUG (real NEO-M9N over UART1) ================= */
static void render_gps_debug(void) {
    if (g.dbg_page == 1) { render_sat_page(); return; }   /* UP/DOWN switches sub-page */
    gps_state_t *gp = gps_get();
    char l[64], t1[20], t2[20], latlon[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);

    /* per-system used / visible counts + max SNR */
    int uG = 0, uB = 0, uA = 0, uL = 0, vG = 0, vB = 0, vA = 0, vL = 0, mx = 0;
    for (int i = 0; i < gp->nsat; i++) {
        gps_sat_t *s = &gp->sats[i];
        if (!s->seen) continue;
        if (s->snr > 0) {
            switch (s->sys) {
            case 'P': vG++; break; case 'B': vB++; break;
            case 'A': vA++; break; case 'L': vL++; break;
            }
        }
        if (s->used) {
            switch (s->sys) {
            case 'P': uG++; break; case 'B': uB++; break;
            case 'A': uA++; break; case 'L': uL++; break;
            }
        }
        if (s->snr > mx) mx = s->snr;
    }

    const char *fxs = gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : (gp->fix_type == 0 ? "NO" : "--"));
    const char *fxl = gp->fix_type == 2 ? "3D FIX" : (gp->fix_type == 1 ? "2D FIX" : (gp->fix_type == 0 ? "NO FIX" : "--"));

    /* header + top-right sky radar (the battery chrome sits just above it) */
    gfx_text(6, 2, "GPS DEBUG", PAL.cyan, 1);
    int sx = 262, sy = 14, SW = 52, SH = 52;
    float cx = sx + SW / 2.0f, cy = sy + SH / 2.0f, R = SW / 2.0f - 5;
    gfx_circle((int)cx, (int)cy, (int)R, PAL.centerline);
    gfx_circle((int)cx, (int)cy, (int)(R / 2), PAL.centerline);
    gfx_hline((int)(cx - R), (int)cy, (int)(2 * R), PAL.centerline);
    gfx_vline((int)cx, (int)(cy - R), (int)(2 * R), PAL.centerline);
    for (int i = 0; i < gp->nsat; i++) {
        gps_sat_t *s = &gp->sats[i];
        if (!s->seen || s->snr <= 0) continue;
        int el = s->el < 0 ? 0 : (s->el > 90 ? 90 : s->el);
        float rr = ((90 - el) / 90.0f) * R;
        float a = (s->az - 90) * (float)M_PI / 180.0f;
        int x = (int)(cx + cosf(a) * rr), y = (int)(cy + sinf(a) * rr);
        uint16_t c = s->snr >= 40 ? PAL.green : (s->snr >= 30 ? PAL.yellow : PAL.red);
        gfx_fill(x - 1, y - 1, s->used ? 3 : 2, s->used ? 3 : 2, c);
    }

    /* big fix line */
    gfx_text(6, 18, fxl, gp->fix_type >= 1 ? PAL.green : PAL.orange, 2);
    u_fmt_f(t1, sizeof t1, gp->hdop, 1);
    snprintf(l, sizeof l, "SAT %d/%d   HDOP %s   %s", gp->sats_used, gp->sats_visible, t1,
             gp->source == 1 ? "I2C" : "UART");
    gfx_text(6, 40, l, PAL.text, 1);

    /* position (shown once, emphasized) */
    u_fmt_f(t1, sizeof t1, (float)fabs(gp->lat), 5);
    u_fmt_f(t2, sizeof t2, (float)fabs(gp->lon), 5);
    snprintf(latlon, sizeof latlon, "%s%c %s%c", t1, gp->lat >= 0 ? 'N' : 'S', t2, gp->lon >= 0 ? 'E' : 'W');
    gfx_text(6, 58, latlon, PAL.cyan, 2);

    /* sentence summaries (no duplicated data) */
    u_fmt_f(t1, sizeof t1, gp->hdop, 1);
    snprintf(l, sizeof l, "GGA q%d  %d/%d  HDOP %s", gp->fix_quality, gp->sats_used, gp->sats_visible, t1);
    gfx_text(6, 80, l, PAL.white, 1);
    snprintf(l, sizeof l, "GSA %s  USED G%d B%d A%d L%d", fxs, uG, uB, uA, uL);
    gfx_text(6, 96, l, PAL.dim, 1);
    snprintf(l, sizeof l, "GSV VIS G%d B%d A%d L%d  MAXSNR %d", vG, vB, vA, vL, mx);
    gfx_text(6, 112, l, PAL.dim, 1);
    u_fmt_f(t1, sizeof t1, gp->speed_kmh, 1);
    snprintf(l, sizeof l, "RMC %s  %s KM/H  UTC %02d:%02d:%02d", gp->has_pos ? "A" : "V", t1,
             gp->utc_h, gp->utc_m, gp->utc_s);
    gfx_text(6, 128, l, PAL.white, 1);

    /* bottom box: live raw NMEA (latest sentence, wrapped over 2 lines) */
    gfx_pxbox(4, 142, CW - 8, CH - 146, PAL.line, 1, PAL.surface);
    const char *raw = gp->raw_n ? gp->raw[0] : "";
    int len = (int)strlen(raw);
    if (len == 0) {
        gfx_text(8, 152, "waiting...", PAL.dim, 1);
    } else {
        for (int r = 0; r < 2; r++) {
            int off = r * 34;
            if (off >= len) break;
            int n = len - off; if (n > 34) n = 34;
            char chunk[36];
            memcpy(chunk, raw + off, n); chunk[n] = 0;
            gfx_text(8, 150 + r * 15, chunk, PAL.curtrace, 1);
        }
    }
    if (gp->txt_toostrong) gfx_text(8, 182, "!! RF INPUT TOO STRONG", PAL.red, 1);
}

/* ================= LOCATION (flat China map + GPS) ================= */
#define LMAP_X 4
#define LMAP_Y 16
#define LMAP_W CN_MAPW
#define LMAP_H CN_MAPH

static void render_location(void) {
    gps_state_t *gp = gps_get();
    char b[40], b2[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("LOCATION", 4);

    int mx = LMAP_X, my = LMAP_Y, mw = LMAP_W, mh = LMAP_H;
    gfx_fill(mx, my, mw, mh, PAL.mapbg);
    /* China province outlines */
    uint16_t border = RGB565(0x3f, 0x6a, 0x52);
    for (int r = 0; r < CN_NRINGS; r++) {
        int off = CN_RINGS[r].off, n = CN_RINGS[r].n;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            gfx_line(mx + CN_PTS[(off + i) * 2], my + CN_PTS[(off + i) * 2 + 1],
                     mx + CN_PTS[(off + j) * 2], my + CN_PTS[(off + j) * 2 + 1], border);
        }
    }
    gfx_rect(mx, my, mw, mh, PAL.mapborder);

    /* current position marker (same projection as the outlines) */
    if (gp->has_pos && (gp->fix_type >= 1 || gp->fix_quality >= 1)) {
        int px = mx + (int)((float)gp->lon * CN_K * CN_S + CN_OX);
        int py = my + (int)(-(float)gp->lat * CN_S + CN_OY);
        gfx_line(px - 6, py, px + 6, py, PAL.yellow);
        gfx_line(px, py - 6, px, py + 6, PAL.yellow);
        gfx_fill(px - 1, py - 1, 3, 3, PAL.red);
        gfx_text(px + 4, py - 12, "YOU", PAL.yellow, 0);
    }

    /* GPS info */
    int ty = my + mh + 4;
    const char *region = (gp->lat > 18 && gp->lat < 54 && gp->lon > 73 && gp->lon < 135) ? "CHINA" : "WORLD";
    u_fmt_f(b, sizeof b, (float)gp->lat, 5);
    u_fmt_f(b2, sizeof b2, (float)gp->lon, 5);
    gfx_text(6, ty, "LAT", PAL.dim, 0);
    gfx_text(34, ty, b, PAL.white, 0);
    gfx_text(120, ty, "LON", PAL.dim, 0);
    gfx_text(148, ty, b2, PAL.white, 0);

    u_fmt_f(b, sizeof b, gp->alt, 1);
    u_fmt_f(b2, sizeof b2, gp->speed_kmh, 1);
    gfx_text(6, ty + 13, "ALT", PAL.dim, 0);
    gfx_text(34, ty + 13, b, PAL.text, 0);
    gfx_text(90, ty + 13, "SPD", PAL.dim, 0);
    gfx_text(118, ty + 13, b2, PAL.text, 0);
    char t[16];
    snprintf(t, sizeof t, "%02d:%02d:%02d", gp->utc_h, gp->utc_m, gp->utc_s);
    gfx_text(170, ty + 13, "UTC", PAL.dim, 0);
    gfx_text(198, ty + 13, t, PAL.text, 0);

    const char *fx = gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : (gp->fix_type == 0 ? "NO" : "--"));
    u_fmt_f(b, sizeof b, gp->hdop, 1);
    snprintf(b2, sizeof b2, "FIX %s  SAT %d  HDOP %s  %s", fx, gp->sats_used, b, region);
    gfx_text(6, ty + 26, b2, gp->fix_type >= 1 ? PAL.green : PAL.orange, 0);
}

/* ================= IMU CALIB ================= */
static void imu_bubble(void) {
    gfx_fill(0, 0, 176, 200, PAL.stage);
    float cx = 88, cy = 100, R = 68;
    float scale = R / 0.15f;
    gfx_circle((int)cx, (int)cy, (int)(0.05f * scale), PAL.gmring);
    gfx_circle((int)cx, (int)cy, (int)(0.10f * scale), PAL.gmring);
    gfx_circle((int)cx, (int)cy, (int)(0.15f * scale), PAL.gmring);
    gfx_hline((int)(cx - R), (int)cy, (int)(2 * R), PAL.gmring);
    gfx_vline((int)cx, (int)(cy - R), (int)(2 * R), PAL.gmring);
    gfx_fill((int)cx - 6, (int)cy - 1, 12, 2, PAL.gmcenter);
    gfx_fill((int)cx - 1, (int)cy - 6, 2, 12, PAL.gmcenter);
    if (g.imu_ns) {
        imu6_t s = g.imu_samples[g.imu_ns - 1];
        float bx = cx + s.ax * scale, by = cy + s.ay * scale;
        float off = hypotf(s.ax, s.ay);
        uint16_t col = off < 0.03f ? PAL.green : (off < 0.08f ? PAL.yellow : PAL.red);
        gfx_fill((int)bx - 3, (int)by - 3, 6, 6, col);
        gfx_fill((int)bx - 1, (int)by - 1, 2, 2, PAL.mapbg);
    }
    int seg = 64;
    float frac = g.imu_progress / IMU_CALIB_MS;
    uint16_t arc = (g.imu_phase == 1) ? PAL.green : PAL.cyan;
    int px = 0, py = 0;
    for (int i = 0; i <= seg; i++) {
        float a = -(float)M_PI / 2 + frac * 2 * (float)M_PI * i / seg;
        int rx = (int)(cx + cosf(a) * (R + 6));
        int ry = (int)(cy + sinf(a) * (R + 6));
        if (i > 0) gfx_line(px, py, rx, ry, arc);
        px = rx; py = ry;
    }
}
static void render_imu_calib(void) {
    char b[40];
    imu_bubble();
    int rx = 182;
    gfx_text(rx, 6, "IMU CALIB", PAL.white, 1);
    gfx_text(rx, 20, "MPU6050 - 6-AXIS", PAL.dim, 1);
    gfx_text(rx, 36, g.imu_phase == 1 ? "DONE" : (g.imu_stable ? "STABLE" : "MOVING"),
             g.imu_phase == 1 ? PAL.green : (g.imu_stable ? PAL.green : PAL.yellow), 2);
    if (g.imu_ns) {
        imu6_t s = g.imu_samples[g.imu_ns - 1];
        char v[40];
        u_fmt_f(v, sizeof v, s.ax, 3); snprintf(b, sizeof b, "A %s", v); gfx_text(rx, 60, b, PAL.text, 0);
        u_fmt_f(v, sizeof v, s.ay, 3); snprintf(b, sizeof b, "  %s", v); gfx_text(rx + 60, 60, b, PAL.text, 0);
        u_fmt_f(v, sizeof v, s.gz, 1); snprintf(b, sizeof b, "GZ %s", v); gfx_text(rx, 74, b, PAL.text, 0);
    }
    gfx_rect(rx, 92, 120, 8, PAL.line);
    gfx_fill(rx + 1, 93, (int)(118 * g.imu_progress / IMU_CALIB_MS), 6, PAL.cyan);
    snprintf(b, sizeof b, "%d%%", (int)(g.imu_progress / IMU_CALIB_MS * 100));
    gfx_text_center(rx + 60, 104, b, PAL.cyan, 1);
    gfx_text(rx, 128, "PUSH DONE  2xPUSH REDO", PAL.dim, 0);
    gfx_text(rx, 142, "BACK SKIP", PAL.dim, 0);
    gfx_text(rx, 178, "PLACE DEVICE LEVEL", PAL.dim, 0);
}

/* ================= dispatch ================= */
void screens_render(void) {
    switch (g.cur) {
    case SC_HOME: render_home(); break;
    case SC_TRACK_EDIT: render_track_edit(); break;
    case SC_SETTINGS: render_settings(); break;
    case SC_MODE_SELECT: render_mode_select(); break;
    case SC_MOUNT_SELECT: render_mount_select(); break;
    case SC_NEW_TRACK_MARK_START: render_record_track(); break;
    case SC_RACING: render_racing_panel(false, &g.engine, g.screenT > 0 ? 0.033f : 0.033f); break;
    case SC_RACING_RECORDED: render_racing_panel(true, &g.engine, 0.033f); break;
    case SC_LAP_SELECT: render_lap_select(); break;
    case SC_SECTION_MARKING: render_section(); break;
    case SC_VEHICLE_SELECT: render_vehicle(); break;
    case SC_TRACK_NAMING: render_naming(); break;
    case SC_TRACK_LIST: render_track_list(); break;
    case SC_HISTORY: render_history(); break;
    case SC_HISTORY_DETAIL: render_history_detail(); break;
    case SC_GPS_DEBUG: render_gps_debug(); break;
    case SC_LOCATION: render_location(); break;
    case SC_SOUND: render_sound(); break;
    case SC_IMU_DEBUG: render_imu_debug(); break;
    case SC_GPS_TRACK: render_gps_track(); break;
    case SC_IMU_CALIB: render_imu_calib(); break;
    default: break;
    }
}

/* ================= enter ================= */
void screens_enter(scid_t id) {
    s_map_ready = false;
    switch (id) {
    case SC_IMU_CALIB:
        g.imu_phase = 0; g.imu_ns = 0; g.imu_progress = 0; g.imu_stable = false;
        g.imu_doneTimer = 0;
        mpu_clear_calibration(&g.imu);
        break;
    case SC_TRACK_EDIT: g.sel = 0; g.entered = false; break;
    case SC_SETTINGS: g.sel = 0; g.entered = false; break;
    case SC_MODE_SELECT: g.sel = 0; break;
    case SC_MOUNT_SELECT: g.sel = g.race_mount; break;
    case SC_NEW_TRACK_MARK_START:
        g.rec_state = 0; g.rec_n = 0; g.rec_dist = 0; g.rec_has_ref = false;
        g.session_nsec = 0;
        break;
    case SC_RACING: case SC_RACING_RECORDED:
        g.panel = 0;
        if (g.engine_valid) {
            engine_set_profile(&g.engine, g.race_mount);
            g.engine.logged = false;
        }
        break;
    case SC_LAP_SELECT:
        g.sel = 0;
        for (int i = 0; i < MAX_LAPS; i++) g.lapChecked[i] = false;
        for (int i = 0; i < g.engine.nLaps; i++) g.lapChecked[i] = true;
        break;
    case SC_SECTION_MARKING:
        g.sec_n = 0; g.sec_prev = 0; g.sec_pos = 0.03f; g.sec_cand = NAN; g.sec_done = false;
        break;
    case SC_VEHICLE_SELECT: g.sel = g.sessionVehicle; break;
    case SC_TRACK_NAMING: {
        g.navail = 0;
        for (char c = 'A'; c <= 'Z'; c++) {
            bool used = false;
            for (int i = 0; i < g.ntracks; i++) if (g.tracks[i].name[0] == c) used = true;
            if (!used) g.avail[g.navail++] = c;
        }
        if (g.navail == 0) { for (char c = 'A'; c <= 'Z'; c++) g.avail[g.navail++] = c; }
        g.sel = 0;
        break;
    }
    case SC_TRACK_LIST: g.cursor = 0; break;
    case SC_SOUND: g.sel = 0; break;
    case SC_IMU_DEBUG: g.i_gmax = 0; break;   /* keep the boot-pose zero offsets */
    case SC_GPS_DEBUG: gps_begin(); gps_restart(); g.dbg_page = 0; break;
    case SC_HISTORY: g.sel = 0; g.entered = false; break;
    case SC_HISTORY_DETAIL:
        g.sel = 0;
        if (g.hist_rec >= 0 && g.hist_rec < g.nlog) {
            histrec_t *r = &g.log[g.hist_rec];
            if (r->hasBest) for (int i = 0; i < r->lapCount; i++) if (r->lapIndex[i] == r->bestIndex) g.sel = i;
        }
        break;
    default: break;
    }
}

/* ================= update ================= */
void screens_update(float dt) {
    engine_t *e = &g.engine;
    switch (g.cur) {
    case SC_IMU_CALIB: {
        if (g.imu_phase == 0) {
            if (g.imu_ns < 100) g.imu_samples[g.imu_ns++] = mpu_sample_static(&g.imu);
            else {
                memmove(&g.imu_samples[0], &g.imu_samples[1], sizeof(imu6_t) * 99);
                g.imu_samples[99] = mpu_sample_static(&g.imu);
            }
            int n = g.imu_ns;
            float stA = 0, stG = 0;
            if (n >= 8) {
                float meanA[3] = {0, 0, 0}, meanG[3] = {0, 0, 0};
                for (int i = 0; i < n; i++) {
                    meanA[0] += g.imu_samples[i].ax; meanA[1] += g.imu_samples[i].ay; meanA[2] += g.imu_samples[i].az;
                    meanG[0] += g.imu_samples[i].gx; meanG[1] += g.imu_samples[i].gy; meanG[2] += g.imu_samples[i].gz;
                }
                for (int k = 0; k < 3; k++) { meanA[k] /= n; meanG[k] /= n; }
                float varA[3] = {0, 0, 0}, varG[3] = {0, 0, 0};
                for (int i = 0; i < n; i++) {
                    float a[3] = {g.imu_samples[i].ax, g.imu_samples[i].ay, g.imu_samples[i].az};
                    float gg[3] = {g.imu_samples[i].gx, g.imu_samples[i].gy, g.imu_samples[i].gz};
                    for (int k = 0; k < 3; k++) { varA[k] += (a[k] - meanA[k]) * (a[k] - meanA[k]); varG[k] += (gg[k] - meanG[k]) * (gg[k] - meanG[k]); }
                }
                for (int k = 0; k < 3; k++) {
                    float sa = sqrtf(varA[k] / n), sg = sqrtf(varG[k] / n);
                    if (sa > stA) stA = sa;
                    if (sg > stG) stG = sg;
                }
            }
            g.imu_stable = (stA < IMU_STABLE_TILT && stG < IMU_STABLE_GYRO);
            g.imu_progress += (g.imu_stable ? dt * 1000.0f : -dt * 600.0f);
            if (g.imu_progress < 0) g.imu_progress = 0;
            if (g.imu_progress >= IMU_CALIB_MS) {
                imu6_t m = {0, 0, 0, 0, 0, 0};
                int n2 = g.imu_ns;
                if (n2) {
                    for (int i = 0; i < n2; i++) {
                        m.ax += g.imu_samples[i].ax; m.ay += g.imu_samples[i].ay; m.az += g.imu_samples[i].az;
                        m.gx += g.imu_samples[i].gx; m.gy += g.imu_samples[i].gy; m.gz += g.imu_samples[i].gz;
                    }
                    m.ax /= n2; m.ay /= n2; m.az /= n2; m.gx /= n2; m.gy /= n2; m.gz /= n2;
                }
                mpu_apply_calibration(&g.imu, m);
                g.imu_phase = 1;
                g.imu_doneTimer = 0.8f;
                app_toast("IMU CALIBRATED");
                if (g.set_snd_calib) buzzer_play_id(SND_CALIB);
            }
        } else if (g.imu_phase == 1) {
            g.imu_doneTimer -= dt;
            if (g.imu_doneTimer <= 0) app_go_home_to(0);
        }
        break;
    }
    case SC_NEW_TRACK_MARK_START: {
        gps_state_t *gp = gps_get();
        if (g.rec_state == 1 && gp->has_pos && gp->fix_type >= 2) {
            if (!g.rec_has_ref) {
                g.rec_has_ref = true;
                g.rec_lat0 = gp->lat; g.rec_lon0 = gp->lon;
                g.rec_n = 0; g.rec_dist = 0;
                g.rec_pts[g.rec_n++] = (pt_t){0, 0};
                g.rec_last = (pt_t){0, 0};
            } else {
                geo_ref_t r = { g.rec_lat0, g.rec_lon0 };
                float mx, my;
                geo_meters(&r, gp->lat, gp->lon, &mx, &my);
                float d = hypotf(mx - g.rec_last.x, my - g.rec_last.y);
                if (d >= 2.0f) {
                    g.rec_dist += d;
                    if (g.rec_n < REC_MAX) g.rec_pts[g.rec_n++] = (pt_t){mx, my};
                    g.rec_last = (pt_t){mx, my};
                }
            }
        }
        break;
    }
    case SC_RACING:
    case SC_RACING_RECORDED:
        if (g.engine_valid) {
            e->speedMul = g.simSpeed;
            if (e->real) {
                gps_state_t *gp = gps_get();
                engine_update_gnss(e, dt, gp->lat, gp->lon, gp->speed_kmh,
                                   gp->has_pos && gp->fix_type >= 1);
            } else {
                engine_update(e, dt);
            }
        }
        break;
    case SC_LAP_SELECT:
        break;
    case SC_GPS_DEBUG:
        gps_poll();
        break;
    case SC_GPS_TRACK: {
        gps_state_t *gp = gps_get();
        if (gp->has_pos && gp->fix_type >= 1) rt_add(rt_get(), gp->lat, gp->lon);
        break;
    }
    case SC_IMU_DEBUG: {
        float ax, ay, az, gx, gy, gz, tp;
        if (mpu6050_read(&ax, &ay, &az, &gx, &gy, &gz, &tp)) {
            g.imu_ok = true;
            g.i_temp = tp;
            g.i_ax = ax - g.i_ox; g.i_ay = ay - g.i_oy; g.i_az = az - g.i_oz;
            g.i_gx = gx - g.i_ogx; g.i_gy = gy - g.i_ogy; g.i_gz = gz - g.i_ogz;
            float a3[3] = {g.i_ax, g.i_ay, g.i_az};
            float h1 = a3[(g.i_vaxis + 1) % 3], h2 = a3[(g.i_vaxis + 2) % 3];
            float mag = sqrtf(h1 * h1 + h2 * h2);   /* horizontal plane only */
            if (mag > g.i_gmax) g.i_gmax = mag;
        }
        break;
    }
    default:
        break;
    }
}

/* ---------- callbacks used by confirm modals ---------- */
static void cb_racing_yes(void) { app_go(SC_LAP_SELECT, 1); }
static void cb_racing_no(void) { app_go_home_to(0); }
static void cb_lapseg_yes(void) { app_go(SC_SECTION_MARKING, 1); }
static void cb_lapseg_no(void) {
    g.session_nsec = 0;
    g.sessionTrack.nsec = 0;
    app_go(SC_VEHICLE_SELECT, 1);
}
static void cb_section_finish_yes(void) {
    if (g.sec_pos > g.sec_prev + 0.02f && g.sec_pos < 0.97f && g.sec_n < MAX_SECTIONS) {
        if (g.sec_pos > g.sec_prev + 0.005f) { g.sec_sections[g.sec_n++] = g.sec_pos; g.sec_prev = g.sec_pos; }
    }
    if (!g.sec_done) {
        g.sec_done = true;
        if (g.sec_n < MAX_SECTIONS) { g.sec_sections[g.sec_n++] = 1.0f; }
        g.session_nsec = g.sec_n;
        for (int i = 0; i < g.sec_n; i++) g.sessionSections[i] = g.sec_sections[i];
        g.sessionTrack.nsec = g.sec_n;
        for (int i = 0; i < g.sec_n; i++) g.sessionTrack.sections[i] = g.sec_sections[i];
    }
    app_go(SC_VEHICLE_SELECT, 1);
}

/* ================= handle ================= */
void screens_handle(const event_t *ev) {
    int d = nav_dir(ev);
    engine_t *e = &g.engine;
    switch (g.cur) {
    case SC_HOME:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) app_go(SC_MODE_SELECT, 1);
        else if (ev->button == BTN_DOWN) app_go_home(1);
        else if (ev->button == BTN_UP) app_go_home(-1);
        break;

    case SC_TRACK_EDIT:
        if (!g.entered) {
            if (d) { app_go_home(d); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) { g.entered = true; }
            else if (ev->button == BTN_BACK) app_go_home_to(0);
        } else {
            if (d && g.ntracks) { g.sel = (g.sel + d + g.ntracks) % g.ntracks; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) app_toast("EDIT ON PHONE APP");
            else if (ev->button == BTN_BACK) g.entered = false;
        }
        break;

    case SC_SETTINGS:
        if (!g.entered) {
            if (d) { app_go_home(d); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_go_home_to(0);
        } else {
            if (d) { g.sel = (g.sel + d + 12) % 12; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) {
                if (g.sel == 0) { g.set_brightness = g.set_brightness >= 100 ? 20 : g.set_brightness + 20; lcd_set_brightness(g.set_brightness); }
                else if (g.sel == 1) g.set_ledcolor = (g.set_ledcolor + 1) % 4;
                else if (g.sel == 2) { g.set_ledbright = g.set_ledbright >= 100 ? 10 : g.set_ledbright + 10; ws2812_set_brightness(g.set_ledbright); }
                else if (g.sel == 3) { g.theme_light = !g.theme_light; theme_set(g.theme_light); }
                else if (g.sel == 4) g.set_wifi = !g.set_wifi;
                else if (g.sel == 5) g.set_bt = !g.set_bt;
                else if (g.sel == 6) app_go(SC_SOUND, 1);
                else if (g.sel == 7) {
                    g.gps_rate_hz = g.gps_rate_hz == 1 ? 5 : g.gps_rate_hz == 5 ? 10 : g.gps_rate_hz == 10 ? 25 : 1;
                    gps_set_rate(g.gps_rate_hz);
                    app_toast(g.gps_rate_hz == 1 ? "GPS 1 HZ" : g.gps_rate_hz == 5 ? "GPS 5 HZ" :
                              g.gps_rate_hz == 10 ? "GPS 10 HZ" : "GPS 25 HZ");
                }
                else if (g.sel == 8) app_go(SC_IMU_DEBUG, 1);
                else if (g.sel == 9) app_go(SC_GPS_TRACK, 1);
                else if (g.sel == 10) app_go(SC_GPS_DEBUG, 1);
                else if (g.sel == 11) app_go(SC_LOCATION, 1);
                if (g.sel <= 5 || g.sel == 7) app_save_settings();   /* persist changes */
            } else if (ev->button == BTN_BACK) g.entered = false;
        }
        break;

    case SC_MODE_SELECT:
        if (d) { g.sel = (g.sel + d + 2) % 2; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) app_go(g.sel == 0 ? SC_NEW_TRACK_MARK_START : SC_TRACK_LIST, 1);
        else if (ev->button == BTN_BACK) app_go(SC_HOME, -1);
        break;

    case SC_MOUNT_SELECT:
        if (d) { g.sel = (g.sel + d + 2) % 2; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            g.race_mount = g.sel;
            if (g.engine_valid) engine_set_profile(e, g.race_mount);
            app_go(g.raceTarget, 1);
        } else if (ev->button == BTN_BACK) app_go(g.raceBack, -1);
        break;

    case SC_NEW_TRACK_MARK_START:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (g.rec_state == 0) {
                if (gps_get()->fix_type < 2) { app_toast("NO GPS 3D FIX"); return; }
                g.rec_state = 1; g.rec_n = 0; g.rec_dist = 0; g.rec_has_ref = false;
                app_toast("RECORDING");
            } else {
                if (g.rec_n < 20 || g.rec_dist < 20.0f) { app_toast("DRIVE A LAP FIRST"); return; }
                track_build_geo(&g.sessionTrack, g.rec_pts, g.rec_n,
                                g.rec_lat0, g.rec_lon0, "NEW", 0);
                g.sessionTrack_valid = true;
                g.sessionVehicle = 0;
                g.session_nsec = 0;
                g.rec_state = 0;
                app_toast("TRACK RECORDED");
                app_go(SC_TRACK_NAMING, 1);
            }
        } else if (ev->button == BTN_DOWN && g.rec_state == 0 && gps_get()->fix_type < 2) {
            app_go(SC_GPS_DEBUG, 1);     /* jump to GPS debug when there is no 3D fix */
        } else if (ev->button == BTN_BACK) {
            if (g.rec_state == 1) { g.rec_state = 0; app_toast("CANCELLED"); }
            else app_go(SC_MODE_SELECT, -1);
        }
        break;

    case SC_RACING:
        if (ev->type == EV_PRESS && (ev->button == BTN_UP || ev->button == BTN_DOWN)) {
            g.panel = (g.panel + (ev->button == BTN_UP ? -1 : 1) + 4) % 4;
            return;
        }
        if (ev->button == BTN_BACK && ev->type == EV_LONG) {
            e->paused = true;
            app_record_race(0);
            app_push_confirm("SAVE THIS RECORD\nAS A NEW TRACK?", "YES", "NO", cb_racing_yes, cb_racing_no);
        }
        break;

    case SC_RACING_RECORDED:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) {
            e->paused = true;
            app_record_race(1);
            app_go(SC_TRACK_LIST, -1);
            return;
        }
        screens_handle_racing_common(ev);
        break;

    case SC_LAP_SELECT: {
        if (d && e->nLaps) { g.sel = (g.sel + d + e->nLaps) % e->nLaps; return; }
        if (ev->type != EV_PRESS && ev->type != EV_DOUBLE) return;
        if (ev->type == EV_PRESS && ev->button == BTN_PUSH && e->nLaps) { g.lapChecked[g.sel] = !g.lapChecked[g.sel]; }
        else if (ev->type == EV_DOUBLE && ev->button == BTN_PUSH) {   /* 2x push = confirm */
            int nsel = 0; for (int i = 0; i < e->nLaps; i++) if (g.lapChecked[i]) nsel++;
            if (nsel < 2) { app_toast("PICK AT LEAST 2 LAPS"); return; }
            /* average selected traces -> sessionTrack */
            const int M = 120;
            pt_t acc[120]; memset(acc, 0, sizeof(acc));
            int used = 0;
            for (int li = 0; li < e->nLaps; li++) {
                if (!g.lapChecked[li]) continue;
                lap_t *lap = &e->laps[li];
                if (lap->nTrace < 4) continue;
                used++;
                for (int i = 0; i < M; i++) {
                    float f = (float)i / M * lap->nTrace;
                    int i0 = ((int)f) % lap->nTrace;
                    int i1 = (i0 + 1) % lap->nTrace;
                    float tt = f - floorf(f);
                    acc[i].x += lap->trace[i0].x + (lap->trace[i1].x - lap->trace[i0].x) * tt;
                    acc[i].y += lap->trace[i0].y + (lap->trace[i1].y - lap->trace[i0].y) * tt;
                }
            }
            if (used) {
                for (int i = 0; i < M; i++) { acc[i].x /= used; acc[i].y /= used; }
                track_build_from_points(&g.sessionTrack, acc, M, "NEW", g.sessionVehicle);
            }
            g.sessionTrack_valid = true;
            app_push_confirm("MARK SECTOR\nSPLITS?", "YES", "NO", cb_lapseg_yes, cb_lapseg_no);
        } else if (ev->button == BTN_BACK) app_go_home_to(0);
        break;
    }

    case SC_SECTION_MARKING:
        if (d) {
            float step = (ev->type == EV_REPEAT || ev->type == EV_LONG) ? 0.05f : 0.02f;
            g.sec_pos = u_clampf(g.sec_pos + d * step, 0.01f, 0.99f);
            return;
        }
        if (ev->type != EV_PRESS && ev->type != EV_DOUBLE) return;
        if (ev->type == EV_PRESS && ev->button == BTN_PUSH) {
            if (!isnan(g.sec_cand)) { g.sec_cand = NAN; app_toast("CANDIDATE CLEARED"); }
            else { g.sec_cand = g.sec_pos; app_toast("CANDIDATE SET"); }
        } else if (ev->type == EV_DOUBLE && ev->button == BTN_PUSH) {   /* 2x push = commit */
            if (!isnan(g.sec_cand)) {
                float f = g.sec_cand;
                if (f > g.sec_prev + 0.005f && g.sec_n < MAX_SECTIONS) {
                    g.sec_sections[g.sec_n++] = f; g.sec_prev = f; g.sec_cand = NAN;
                    if (g.sec_prev >= 0.985f) {
                        cb_section_finish_yes();
                        return;
                    }
                    app_toast("SECTION MARKED");
                } else {
                    g.sec_cand = NAN;
                    app_toast("TOO CLOSE - CLEARED");
                    goto section_prompt;
                }
                return;
            }
        section_prompt:;
            {
                bool extra = (g.sec_pos > g.sec_prev + 0.02f && g.sec_pos < 0.97f);
                app_push_confirm(extra ? "CURRENT POS TO END\nAS LAST SECTION?" : "REST OF LAP\nAS LAST SECTION?",
                                 "OK", "BACK", cb_section_finish_yes, NULL);
            }
        } else if (ev->button == BTN_BACK) {
            if (!isnan(g.sec_cand)) { g.sec_cand = NAN; app_toast("CANDIDATE CLEARED"); return; }
            app_go(SC_LAP_SELECT, -1);
        }
        break;

    case SC_VEHICLE_SELECT:
        if (d) { g.sel = (g.sel + d + 4) % 4; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { g.sessionVehicle = g.sel; app_go(SC_TRACK_NAMING, 1); }
        else if (ev->button == BTN_BACK) app_go(SC_LAP_SELECT, -1);
        break;

    case SC_TRACK_NAMING:
        if (d && g.navail) { g.sel = (g.sel + d + g.navail) % g.navail; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            char name[4] = {g.avail[g.sel], 0, 0, 0};
            track_t *t = &g.sessionTrack;
            strncpy(t->name, name, sizeof(t->name) - 1);
            t->vehicle = g.sessionVehicle;
            t->nsec = g.session_nsec;
            for (int i = 0; i < g.session_nsec; i++) t->sections[i] = g.sessionSections[i];
            t->hasBest = false;
            if (g.ntracks < MAX_TRACKS) g.tracks[g.ntracks++] = *t;
            app_toast("TRACK SAVED");
            g.sessionTrack_valid = false;
            g.session_nsec = 0;
            app_go_home_to(0);
        } else if (ev->button == BTN_BACK) app_go(SC_VEHICLE_SELECT, -1);
        break;

    case SC_TRACK_LIST:
        if (d && g.ntracks) { g.cursor = (g.cursor + d + g.ntracks) % g.ntracks; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (!g.ntracks) return;
            track_t *t = &g.tracks[g.cursor];
            engine_init(e, t, true, &g.imu);
            g.engine_valid = true;
            g.raceTarget = SC_RACING_RECORDED;
            g.raceBack = SC_TRACK_LIST;
            app_go(SC_MOUNT_SELECT, 1);
        } else if (ev->button == BTN_BACK) app_go(SC_HOME, -1);
        break;

    case SC_HISTORY:
        if (!g.entered) {
            if (d) { app_go_home(d); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_go_home_to(0);
        } else {
            if (d && g.nlog) { g.sel = (g.sel + d + g.nlog) % g.nlog; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH && g.nlog) { g.hist_rec = g.nlog - 1 - g.sel; app_go(SC_HISTORY_DETAIL, 1); }
            else if (ev->button == BTN_BACK) g.entered = false;
        }
        break;

    case SC_HISTORY_DETAIL:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) { app_go(SC_HISTORY, -1); return; }
        if (d && g.hist_rec >= 0 && g.hist_rec < g.nlog) {
            int n = g.log[g.hist_rec].lapCount;
            if (n) g.sel = (g.sel + d + n) % n;
            return;
        }
        break;

    case SC_GPS_DEBUG:
        if (ev->type == EV_PRESS && (ev->button == BTN_UP || ev->button == BTN_DOWN)) {
            g.dbg_page = (g.dbg_page + 1) % 2;   /* switch debug sub-page */
            return;
        }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { gps_next_baud(); app_toast("GPS NEXT BAUD"); }
        else if (ev->button == BTN_BACK) app_go(g.prev_cur, -1);   /* return to where we came from */
        break;

    case SC_LOCATION:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_BACK) app_go(SC_SETTINGS, -1);
        break;

    case SC_IMU_DEBUG:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            g.i_ox += g.i_ax; g.i_oy += g.i_ay; g.i_oz += g.i_az;
            g.i_ogx += g.i_gx; g.i_ogy += g.i_gy; g.i_ogz += g.i_gz;
            g.i_gmax = 0;
            app_toast("IMU TARE");
        } else if (ev->button == BTN_BACK) app_go(SC_SETTINGS, -1);
        break;

    case SC_GPS_TRACK:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { rt_reset(rt_get()); app_toast("GPS TRACK CLEARED"); }
        else if (ev->button == BTN_BACK) app_go(SC_SETTINGS, -1);
        break;

    case SC_SOUND:
        if (d) { g.sel = (g.sel + d + 4) % 4; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (g.sel == 0) {
                g.set_sound_vol = g.set_sound_vol >= 100 ? 0 : g.set_sound_vol + 10;
                buzzer_set_volume(g.set_sound_vol);
                buzzer_play_id(SND_FIX);              /* preview */
            } else if (g.sel == 1) {
                g.set_snd_confirm = !g.set_snd_confirm;
                if (g.set_snd_confirm) buzzer_play_id(SND_CONFIRM);
            } else if (g.sel == 2) {
                g.set_snd_calib = !g.set_snd_calib;
                if (g.set_snd_calib) buzzer_play_id(SND_CALIB);
            } else {
                g.set_snd_fix = !g.set_snd_fix;
                if (g.set_snd_fix) buzzer_play_id(SND_FIX);
            }
            app_save_settings();
        } else if (ev->button == BTN_BACK) app_go(SC_SETTINGS, -1);
        break;

    case SC_IMU_CALIB:
        if (ev->type != EV_PRESS && ev->type != EV_DOUBLE) return;
        if (ev->type == EV_PRESS && ev->button == BTN_PUSH) {
            imu6_t m = {0, 0, 0, 0, 0, 0};
            int n2 = g.imu_ns;
            if (n2) {
                for (int i = 0; i < n2; i++) {
                    m.ax += g.imu_samples[i].ax; m.ay += g.imu_samples[i].ay; m.az += g.imu_samples[i].az;
                    m.gx += g.imu_samples[i].gx; m.gy += g.imu_samples[i].gy; m.gz += g.imu_samples[i].gz;
                }
                m.ax /= n2; m.ay /= n2; m.az /= n2; m.gx /= n2; m.gy /= n2; m.gz /= n2;
            }
            mpu_apply_calibration(&g.imu, m);
            g.imu_phase = 1; g.imu_doneTimer = 0.8f; g.imu_progress = IMU_CALIB_MS;
            app_toast("IMU CALIBRATED");
            if (g.set_snd_calib) buzzer_play_id(SND_CALIB);
        } else if (ev->type == EV_DOUBLE && ev->button == BTN_PUSH) {   /* 2x push = resample */
            g.imu_ns = 0; g.imu_progress = 0; g.imu_phase = 0;
            mpu_clear_calibration(&g.imu);
            app_toast("RESAMPLING");
        } else if (ev->button == BTN_BACK) app_go_home_to(0);
        break;

    default: break;
    }
}

/* racing panel switching shared by RACING and RACING_RECORDED */
void screens_handle_racing_common(const event_t *ev) {
    if (ev->type == EV_PRESS && (ev->button == BTN_UP || ev->button == BTN_DOWN)) {
        g.panel = (g.panel + (ev->button == BTN_UP ? -1 : 1) + 4) % 4;
        return;
    }
    if (ev->button == BTN_BACK && ev->type == EV_LONG) {
        g.engine.paused = true;
        app_record_race(1);
        app_go_home_to(0);
    }
}

/* ================= chrome ================= */
void screens_chrome(void) {
    char b[80];
    g.statusColor = PAL.dim;
    for (int i = 0; i < LED_COUNT; i++) g.leds[i] = 0;
    switch (g.cur) {
    case SC_HOME: snprintf(g.status, sizeof g.status, "HOME - START RACING"); break;
    case SC_TRACK_EDIT: snprintf(g.status, sizeof g.status, g.entered ? "TRACK EDIT - EDITING" : "TRACK EDIT - PUSH TO EDIT"); break;
    case SC_SETTINGS: snprintf(g.status, sizeof g.status, g.entered ? "SETTINGS - ADJUST" : "SETTINGS - PUSH TO ENTER"); break;
    case SC_MODE_SELECT: snprintf(g.status, sizeof g.status, "MODE SELECT"); break;
    case SC_MOUNT_SELECT: snprintf(g.status, sizeof g.status, g.sel == 0 ? "MOUNT - FIXED (TRUST IMU)" : "MOUNT - NOT FIXED (TRUST GPS)"); break;
    case SC_NEW_TRACK_MARK_START:
        if (g.rec_state == 1) snprintf(g.status, sizeof g.status, "RECORDING  %.0f M  %d PTS", g.rec_dist, g.rec_n);
        else snprintf(g.status, sizeof g.status, gps_get()->fix_type >= 2 ? "NEW TRACK - PUSH AT START" : "NEW TRACK - WAIT 3D FIX");
        break;
    case SC_RACING:
    case SC_RACING_RECORDED: {
        engine_t *e = &g.engine;
        u_fmt_time(b, sizeof b, e->elapsed);
        snprintf(g.status, sizeof g.status, "%s %d/4   LAP %d   %s", RACE_PANELS[g.panel], g.panel + 1, e->lap, b);
        for (int i = 0; i < LED_COUNT; i++) g.leds[i] = e->leds[i];
        break;
    }
    case SC_LAP_SELECT: { int n = 0; for (int i = 0; i < g.engine.nLaps; i++) if (g.lapChecked[i]) n++; snprintf(g.status, sizeof g.status, "SELECT LAPS - %d PICKED", n); break; }
    case SC_SECTION_MARKING: snprintf(g.status, sizeof g.status, "SECTION %d - %d%%", g.sec_n + 1, (int)(g.sec_pos * 100)); break;
    case SC_VEHICLE_SELECT: snprintf(g.status, sizeof g.status, "VEHICLE - %s", VEH_NAMES[g.sel]); break;
    case SC_TRACK_NAMING: snprintf(g.status, sizeof g.status, "NAME TRACK"); break;
    case SC_TRACK_LIST: snprintf(g.status, sizeof g.status, "TRACK LIST - %d", g.ntracks); break;
    case SC_HISTORY: snprintf(g.status, sizeof g.status, g.entered ? "HISTORY - BROWSE" : "HISTORY - PUSH TO ENTER"); break;
    case SC_HISTORY_DETAIL: snprintf(g.status, sizeof g.status, "RACE DETAIL"); break;
    case SC_IMU_CALIB: snprintf(g.status, sizeof g.status, "IMU CALIB - %s - %d%%", g.imu_phase ? "DONE" : (g.imu_stable ? "STABLE" : "MOVING"), (int)(g.imu_progress / IMU_CALIB_MS * 100));
        g.statusColor = g.imu_stable ? PAL.green : PAL.yellow; break;
    case SC_LOCATION: {
        gps_state_t *gp = gps_get();
        snprintf(g.status, sizeof g.status, "LOCATION  FIX %s  SAT %d  HDOP %.1f",
                 gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : "NO"), gp->sats_used, gp->hdop);
        g.statusColor = gp->fix_type >= 1 ? PAL.green : PAL.orange;
        break;
    }
    case SC_SOUND:
        snprintf(g.status, sizeof g.status, "SOUND  VOL %d%%  %s", g.set_sound_vol,
                 g.set_sound_vol ? "ON" : "MUTED");
        g.statusColor = g.set_sound_vol ? PAL.green : PAL.dim;
        break;
    case SC_IMU_DEBUG:
        snprintf(g.status, sizeof g.status, "IMU DEBUG  MPU6050 %s", g.imu_ok ? "OK" : "NOT FOUND");
        g.statusColor = g.imu_ok ? PAL.green : PAL.red;
        break;
    case SC_GPS_TRACK:
        snprintf(g.status, sizeof g.status, "GPS TRACK  %.1f M  %d PTS", rt_get()->dist, rt_get()->n);
        g.statusColor = gps_get()->fix_type >= 1 ? PAL.green : PAL.dim;
        break;
    case SC_GPS_DEBUG: {
        gps_state_t *gp = gps_get();
        if (gp->source == 1)
            snprintf(g.status, sizeof g.status, "GPS I2C  RX %lu  NMEA %lu  PG%d/2 UP-DN",
                     (unsigned long)gp->bytes_rx, (unsigned long)gp->sentences, g.dbg_page + 1);
        else
            snprintf(g.status, sizeof g.status, "GPS UART%d  RX %lu  NMEA %lu  PG%d/2 UP-DN",
                     gp->baud, (unsigned long)gp->bytes_rx, (unsigned long)gp->sentences, g.dbg_page + 1);
        g.statusColor = gp->sentences ? PAL.green : PAL.dim;
        break;
    }
    default: g.status[0] = 0; break;
    }
}
