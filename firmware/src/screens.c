/* All UI screens + the state machine handlers (port of js/screens.js).
 * Rendering uses CONTENT-local coordinates: (0,0) is the top-left of the
 * 360x200 content area (the shell adds the LED/status chrome).
 */
#include "app.h"
#include "gfx.h"
#include "map.h"
#include "theme.h"
#include "log_store.h"
#include "race_store.h"
#include "tstore.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "util.h"
#include "lcd.h"
#include "gps.h"
#include "ws2812.h"
#include "buzzer.h"
#include "mpu6050.h"
#include "geo.h"
#include "tstore.h"
#include "netcfg.h"
#include "nav.h"
#include "btmap.h"
#include "china_map.h"
#include "home_logos.h"
#include "race_store.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CW LCD_W
#define CH CONTENT_H
#define HINT_Y (CH - 30)   /* generic bottom hint, above the status pill */

static tmap_t s_map;
static bool   s_map_ready;

static const char *RACE_PANELS[4] = {"TRACK MODE", "TIME MODE", "G FORCE MODE", "SECTION MODE"};

static const char *VEH_NAMES[4] = {"KART", "4-STROKE", "KART+", "RACE"};
#define HOMEF1_RED    RGB565(0xe1, 0x06, 0x00)
#define HOMEF1_TEAL   RGB565(0x27, 0xf4, 0xd2)
#define HOMEF1_ORANGE RGB565(0xff, 0x80, 0x00)
#define HOMEF1_SILVER RGB565(0xd5, 0xd7, 0xda)
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

static int lr_dir(const event_t *ev) {
    if (ev->type != EV_PRESS && ev->type != EV_LONG && ev->type != EV_REPEAT) return 0;
    if (ev->button == BTN_LEFT) return -1;
    if (ev->button == BTN_RIGHT) return 1;
    return 0;
}

/* screens that need a live, high-rate GNSS fix; everything else uses 1 Hz */
static bool screen_needs_gps(scid_t id) {
    switch (id) {
    case SC_NAV: case SC_PERF: case SC_GPS_DEBUG: case SC_LOCATION:
    case SC_RACING: case SC_RACING_RECORDED: case SC_NEW_TRACK_MARK_START:
        return true;
    default:
        return false;
    }
}

/* All race states run at 25 Hz; other GNSS pages keep the user setting. */
static int screen_gps_hz(scid_t id) {
    if (id == SC_RACING || id == SC_RACING_RECORDED || id == SC_NEW_TRACK_MARK_START)
        return 25;
    if (screen_needs_gps(id)) return g.gps_rate_hz;
    return 1;
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
static void draw_home_logo(int x, int y, int idx) {
    if (idx < 0 || idx >= 4) return;
    const home_logo_t *logo = &HOME_LOGOS[idx];
    for (int py = 0; py < 64; py++) {
        for (int px = 0; px < 64; px++) {
            uint8_t v = logo->idx[py * 64 + px];
            if (v) gfx_pixel(x + px, y + py, logo->pal[v]);
        }
    }
}


/* ================= Formula 1 visual language ================= */
static bool f1_ui(void) { return g.ui_style != 0; }

static void f1_checker(int x, int y, int w, int h, int cell, uint16_t a, uint16_t b) {
    for (int yy = 0; yy < h; yy += cell) {
        for (int xx = 0; xx < w; xx += cell) {
            uint16_t c = (((xx / cell) + (yy / cell)) & 1) ? a : b;
            gfx_fill(x + xx, y + yy, cell, cell, c);
        }
    }
}

static uint16_t f1_dark(uint16_t c, int pct) {
    int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    r = r * pct / 100; g = g * pct / 100; b = b * pct / 100;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Selected card: a light team-colour wash, not a near-black tint. */
static uint16_t f1_selected(uint16_t c) {
    int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    int pr = (PAL.panel >> 11) & 0x1f, pg = (PAL.panel >> 5) & 0x3f, pb = PAL.panel & 0x1f;
    r = (r * 55 + 31 * 15 + pr * 30) / 100;
    g = (g * 55 + 63 * 15 + pg * 30) / 100;
    b = (b * 55 + 31 * 15 + pb * 30) / 100;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void f1_panel(int x, int y, int w, int h, bool on, uint16_t accent) {
    if (on) {
        /* Drop shadow + a deeper accent wash make the selected card read as
         * a physical foreground tile instead of just another outline. */
        gfx_fill(x + 5, y + 6, w, h, f1_dark(accent, 18));
        gfx_fill(x + 2, y + 3, w, h, f1_dark(accent, 8));
    }
    gfx_fill(x, y, w, h, on ? f1_selected(accent) : PAL.panel);
    gfx_rect(x, y, w, h, on ? accent : PAL.line);
    if (on) gfx_rect(x + 1, y + 1, w - 2, h - 2, accent);
    gfx_fill(x, y, 4, h, accent);
    gfx_fill(x + 4, y, w - 4, on ? 3 : 2, on ? PAL.white : PAL.line2);
    /* small clipped top-right corner, the F1 broadcast-card cue */
    for (int i = 0; i < 7; i++) gfx_hline(x + w - 8 + i, y + i, 8 - i, PAL.stage);
}

static void f1_header(const char *title) {
    gfx_fill(0, 0, CW, 4, PAL.cyan);
    f1_checker(CW - 34, 7, 28, 12, 4, PAL.white, PAL.stage);
    gfx_text(8, 8, title, PAL.white, 2);
}

static void f1_icon(int kind, int x, int y, uint16_t accent) {
    gfx_rect(x, y, 55, 55, PAL.line2);
    gfx_fill(x + 1, y + 1, 53, 53, PAL.stage);
    int cx = x + 27, cy = y + 27;
    if (kind == 0) {                 /* checkered flag */
        gfx_fill(cx - 14, cy - 17, 3, 34, accent);
        f1_checker(cx - 7, cy - 16, 20, 20, 4, PAL.white, PAL.line2);
    } else if (kind == 1) {          /* compass / nav */
        int ax[4] = {cx, cx + 12, cx, cx - 12};
        int ay[4] = {cy - 20, cy + 15, cy + 7, cy + 15};
        gfx_poly_fill(ax, ay, 4, accent);
        int bx[4] = {cx, cx + 4, cx, cx - 4};
        int by[4] = {cy - 10, cy + 7, cy + 3, cy + 7};
        gfx_poly_fill(bx, by, 4, PAL.stage);
    } else if (kind == 2) {          /* music note */
        gfx_disc(cx - 7, cy + 9, 10, accent);
        gfx_fill(cx + 1, cy - 17, 4, 25, accent);
        int ax[3] = {cx + 1, cx + 20, cx + 1};
        int ay[3] = {cy - 17, cy - 10, cy - 4};
        gfx_poly_fill(ax, ay, 3, accent);
    } else {                         /* setup / gear */
        gfx_circle(cx, cy, 13, accent);
        gfx_circle(cx, cy, 9, accent);
        gfx_disc(cx, cy, 4, accent);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * (float)M_PI / 4.0f;
            gfx_line(cx + (int)(cosf(a) * 11), cy + (int)(sinf(a) * 11),
                     cx + (int)(cosf(a) * 20), cy + (int)(sinf(a) * 20), accent);
        }
    }
}

static void render_home_f1(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    /* carbon diagonal texture, intentionally very low contrast */
    for (int i = -CH; i < CW; i += 12) {
        gfx_line(i, 0, i + CH, CH, PAL.line2);
    }
    f1_header("RACEBRICK");
    gfx_text(10, 32, "MAIN MENU", PAL.dim, 1);

    static const char *items[4] = {"RACE", "NAV", "MEDIA", "SET"};
    static const char *nums[4] = {"01", "02", "03", "04"};
    static const uint16_t accents[4] = {HOMEF1_RED, HOMEF1_TEAL, HOMEF1_ORANGE, HOMEF1_SILVER};
    const int gap = 8, margin = 10;
    const int w = (CW - margin * 2 - gap) / 2;
    const int h = 78;
    const int ys[2] = {44, 130};
    for (int i = 0; i < 4; i++) {
        int col = i & 1, row = i >> 1;
        int x = margin + col * (w + gap);
        int y = ys[row];
        bool on = i == g.homeIndex;
        f1_panel(x, y, w, h, on, accents[i]);
        f1_icon(i, x + 7, y + 12, accents[i]);
        gfx_text(x + 72, y + 10, nums[i], accents[i], 2);
        gfx_text(x + 72, y + 33, items[i], on ? PAL.white : PAL.text, 2);
        gfx_text(x + 72, y + 55, on ? "SELECTED" : "STANDBY", on ? accents[i] : PAL.dim, 0);
    }
    gfx_text_center(CW / 2, CH - 14, "UP/DN/L/R SELECT   PUSH OPEN", PAL.dim, 0);
}

static void render_home(void) {
    if (f1_ui()) { render_home_f1(); return; }
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(10, 7, "RACEBRICK", PAL.white, 2);
    gfx_fill(10, 27, 102, 3, PAL.cyan);
    gfx_fill(114, 27, 18, 3, PAL.dim);

    static const char *items[4] = {"RACE", "NAV", "MEDIA", "SET"};
    const int gap = 8;
    const int margin = 10;
    const int w = (CW - margin * 2 - gap) / 2;
    const int h = 80;
    const int ys[2] = {38, 126};
    for (int i = 0; i < 4; i++) {
        int col = i & 1, row = i >> 1;
        int x = margin + col * (w + gap);
        int y = ys[row];
        bool on = i == g.homeIndex;
        panel_box(x, y, w, h, on, false);
        if (on) {
            gfx_fill(x + 3, y + 3, 4, h - 6, PAL.cyan);
            gfx_fill(x + 3, y + 3, w - 6, 3, PAL.cyan);
        }
        draw_home_logo(x + 6, y + 8, i);
        gfx_text(x + 76, y + 33, items[i], on ? PAL.cyan : PAL.white, 2);
    }
    gfx_text_center(CW / 2, 214, "UP/DN/L/R SELECT   PUSH OPEN", PAL.dim, 0);
}

static void render_race_menu(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("RACE", 6);
    static const char *items[4] = {"START RACE", "RACE LOG", "TRACKS", "PERFORMANCE"};
    static const char *subs[4] = {"NEW / RECORDED", "PAST RACES", "TRACK MANAGEMENT", "0-100 / 100-0"};
    const int h = 40;
    int y = 38;
    for (int i = 0; i < 4; i++) {
        bool on = i == g.sel;
        panel_box(18, y, CW - 36, h - 4, on, false);
        gfx_text(30, y + 7, items[i], on ? PAL.cyan : PAL.text, 2);
        gfx_text(30, y + 24, subs[i], PAL.dim, 0);
        y += h;
    }
    hint_c("UP/DOWN SELECT - PUSH OPEN - BACK", HINT_Y);
}

static void render_nav_menu(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("NAVIGATION", 6);
    static const char *items[2] = {"NAVIGATION", "SATELLITE POSITION"};
    static const char *subs[2] = {"PHONE MAP / ROUTE", "LIVE LOCATION IMAGERY"};
    const int h = 48;
    int y = 56;
    for (int i = 0; i < 2; i++) {
        bool on = i == g.sel;
        panel_box(18, y, CW - 36, h - 4, on, false);
        gfx_text(30, y + 9, items[i], on ? PAL.cyan : PAL.text, 2);
        gfx_text(30, y + 28, subs[i], PAL.dim, 0);
        y += h;
    }
    hint_c("UP/DOWN SELECT - PUSH OPEN - BACK", HINT_Y);
}

/* ================= PERFORMANCE (0-100 / 100-0) ================= */
static float s_perf_g = 0;
static float s_perf_prev_spd = 0;

/* Signed forward acceleration is needed for custom ranges: accelerating
 * custom ranges trigger on positive G, decelerating ranges on negative G. */
static bool perf_read_g2(float *gmag, float *gfwd) {
    float ax, ay, az, gx, gy, gz, tp;
    if (!g.imu_ok || !mpu6050_read(&ax, &ay, &az, &gx, &gy, &gz, &tp)) return false;
    float hr, hf, hu;
    if (!app_imu_axes(ax, ay, az, &hr, &hf, &hu)) return false;
    *gfwd = hf;
    *gmag = sqrtf(hr * hr + hf * hf);
    return true;
}

static bool perf_read_g(float *gmag) {
    float gfwd;
    return perf_read_g2(gmag, &gfwd);
}

static void perf_update(float dt) {
    (void)dt;
    gps_state_t *gp = gps_get();
    float spd = gp->speed_kmh;

    float live_g = 0, live_fwd = 0;
    perf_read_g2(&live_g, &live_fwd);
    s_perf_g = live_g;
    float prev_spd = s_perf_prev_spd;
    s_perf_prev_spd = spd;

    if (g.perf_state == 1) {
        /* Fast-poll the IMU for a precise launch/brake timestamp (~2 ms).
         * No debounce: a single sample over threshold fires, like the original
         * MotoNav/race timer behaviour. */
        for (int k = 0; k < 8 && g.perf_state == 1; k++) {
            float gm;
            if (perf_read_g(&gm)) {
                s_perf_g = gm;
                float th = (float)g.perf_trig_g100 / 100.0f;
                bool trig = false;
                if (g.perf_mode == 0) {
                    trig = gm > th;
                } else if (g.perf_mode == 1) {
                    trig = (spd > 30.0f && gm > PERF_BRAKE_G);
                } else {
                    int a = g.perf_speed_a, b = g.perf_speed_b;
                    bool accel = a < b;
                    if (accel) {
                        trig = ((prev_spd <= (float)a && spd > (float)a) ||
                                (spd >= (float)a && spd <= (float)a + 5.0f)) &&
                               (live_fwd > th);
                    } else {
                        trig = ((prev_spd >= (float)a && spd < (float)a) ||
                                (spd <= (float)a && spd >= (float)a - 5.0f)) &&
                               (live_fwd < -PERF_BRAKE_G);
                    }
                    if (a == b) trig = false;
                }
                if (trig) {
                    g.perf_t0_us = esp_timer_get_time();
                    g.perf_speed0 = spd;
                    g.perf_time = 0;
                    g.perf_state = 2;
                    ESP_LOGI("perf", "trigger %.2fG mode=%d spd=%.1f", gm, g.perf_mode, spd);
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    } else if (g.perf_state == 2) {
        g.perf_time = (float)(esp_timer_get_time() - g.perf_t0_us) / 1e6f;
        bool done;
        if (g.perf_mode == 0) done = spd >= 100.0f;
        else if (g.perf_mode == 1) done = spd <= 1.0f;
        else {
            int a = g.perf_speed_a, b = g.perf_speed_b;
            done = (a < b) ? (spd >= (float)b) : (spd <= (float)b);
        }
        if (done) g.perf_state = 3;
        else if (g.perf_time > 30.0f) { g.perf_state = 0; g.perf_time = 0; }
    }
}

static void render_perf_mode_boxes(void) {
    const int mw = 94, mx[3] = {10, 110, 210};
    static const char *modes[3] = {"0-100", "100-0", "CUSTOM"};
    bool flash = !g.entered && ((int)(g.now / 260) % 2 == 0);
    for (int i = 0; i < 3; i++) {
        int selected = g.entered ? g.perf_mode : g.perf_mode_sel;
        bool on = selected == i;
        panel_box(mx[i], 24, mw, 27, on, false);
        if (on) {
            gfx_fill(mx[i] + 2, 26, mw - 4, 23,
                     (g.entered || flash) ? PAL.seltint : PAL.surface2);
            gfx_rect(mx[i] + 1, 25, mw - 2, 25, on ? PAL.cyan : PAL.line);
        }
        gfx_text_center(mx[i] + mw / 2, 34, modes[i], on ? PAL.cyan : PAL.dim, 1);
    }
}

static void render_perf_edit(void) {
    int field = g.perf_edit_field;
    int value = field == 0 ? g.perf_speed_a : g.perf_speed_b;
    int old = g.perf_edit_old;
    char vs[16], os[16];
    snprintf(vs, sizeof vs, "%d", value);
    snprintf(os, sizeof os, "%d", old);
    gfx_dim(0, 0, CW, CH, PAL.statusbg);
    gfx_pxbox(44, 52, 232, 132, PAL.cyan, 1, PAL.panel);
    gfx_text_center(CW / 2, 64, field == 0 ? "START SPEED" : "END SPEED", PAL.dim, 1);
    int cy = 112;
    float a = g.perf_edit_anim;
    int oldY, newY;
    if (g.perf_edit_dir > 0) {
        oldY = cy - (int)((1.0f - a) * 24.0f);
        newY = cy + (int)(a * 24.0f);
    } else {
        oldY = cy + (int)((1.0f - a) * 24.0f);
        newY = cy - (int)(a * 24.0f);
    }
    if (a > 0.02f) gfx_text_center(CW / 2, oldY - 12, os, PAL.glyphoff, 4);
    gfx_text_center(CW / 2, newY - 12, vs, PAL.white, 5);
    gfx_text_center(CW / 2, 158, "UP/DN ADJUST", PAL.dim, 0);
    gfx_text_center(CW / 2, 170, "PUSH OK - BACK CANCEL", PAL.dim, 0);
}

static void render_perf(void) {
    gps_state_t *gp = gps_get();
    char b[32], b2[32];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(6, 2, "PERFORMANCE", PAL.cyan, 1);
    render_perf_mode_boxes();

    if (!g.entered) {
        gfx_text_center(CW / 2, 100, "SELECT MODE", PAL.text, 2);
        gfx_text_center(CW / 2, 132,
                        g.perf_mode_sel == 0 ? "0-100" : g.perf_mode_sel == 1 ? "100-0" : "CUSTOM",
                        PAL.cyan, 2);
        gfx_text_center(CW / 2, HINT_Y, "L/R SELECT MODE - PUSH ENTER - BACK", PAL.dim, 0);
        return;
    }

    if (g.perf_mode == 2 && g.perf_state == 0) {
        /* Custom setup: values + CONFIRM only, no speed/time. */
        bool fa = g.perf_custom_focus == 0;
        bool fb = g.perf_custom_focus == 1;
        bool fc = g.perf_custom_focus == 2;
        panel_box(10, 64, 145, 54, fa, false);
        panel_box(165, 64, 145, 54, fb, false);
        if (fa) gfx_rect(9, 63, 147, 56, PAL.cyan);
        if (fb) gfx_rect(164, 63, 147, 56, PAL.cyan);
        gfx_text(20, 72, "START", fa ? PAL.white : PAL.dim, 1);
        gfx_text(175, 72, "END", fb ? PAL.white : PAL.dim, 1);
        snprintf(b, sizeof b, "%d", g.perf_speed_a);
        gfx_text_right(146, 86, b, fa ? PAL.cyan : PAL.text, 3);
        snprintf(b, sizeof b, "%d", g.perf_speed_b);
        gfx_text_right(301, 86, b, fb ? PAL.cyan : PAL.text, 3);
        gfx_text_center(CW / 2, 126,
                        g.perf_speed_a < g.perf_speed_b ? "ACCELERATE" : "DECELERATE",
                        PAL.green, 0);
        panel_box(60, 146, 200, 34, fc, false);
        if (fc) gfx_rect(59, 145, 202, 36, PAL.cyan);
        gfx_text_center(CW / 2, 158, "CONFIRM", fc ? PAL.cyan : PAL.text, 2);
        gfx_text_center(CW / 2, HINT_Y, "L/R SELECT  PUSH EDIT  UP/DN FOCUS  UP@START MODE", PAL.dim, 0);
        if (g.perf_edit) render_perf_edit();
        return;
    }

    int sy = 80;
    gfx_text(18, 68, "SPEED", PAL.dim, 0);
    snprintf(b, sizeof b, "%d", (int)lroundf(gp->speed_kmh));
    gfx_text(14, 92, b, PAL.white, 5);
    gfx_text(72, 138, "KM/H", PAL.dim, 1);
    gfx_text(186, 68, "TIME", PAL.dim, 0);
    float t = (g.perf_state == 2) ? (float)(esp_timer_get_time() - g.perf_t0_us) / 1e6f : g.perf_time;
    snprintf(b, sizeof b, "%.3f", t);
    gfx_text(182, 92, b, g.perf_state == 3 ? PAL.gold : PAL.white, 4);

    bool has_fix = (gp->fix_type >= 1 && gp->has_pos);
    const char *st;
    uint16_t sc;
    if (!has_fix) { st = "NEED GPS FIX"; sc = PAL.red; }
    else switch (g.perf_state) {
    case 1: st = (g.perf_mode == 0 || (g.perf_mode == 2 && g.perf_speed_a < g.perf_speed_b)) ? "ARMED - ACCEL!" : "ARMED - BRAKE!"; sc = PAL.yellow; break;
    case 2: st = "RUNNING"; sc = PAL.cyan; break;
    case 3: st = "DONE"; sc = PAL.gold; break;
    default: st = "PUSH TO ARM"; sc = PAL.dim; break;
    }
    snprintf(b, sizeof b, "G %.2f / %.2f", s_perf_g, g.perf_trig_g100 / 100.0f);
    gfx_text(186, 140, b, PAL.dim, 0);
    gfx_text_center(CW / 2, 170, st, sc, 1);
    if (!has_fix) gfx_text(6, HINT_Y, "PUSH: GPS DEBUG (ACQUIRE) - BACK", PAL.orange, 0);
    else gfx_text(6, HINT_Y, "PUSH ARM/CANCEL - UP MODE - BACK", PAL.dim, 0);
    if (g.perf_edit) render_perf_edit();
}

/* ================= LOCATION (real-time pixel road map) ================= */
static int nav_style_idx(void) {
    if (g.nav_mode == 1) return 1;                 /* satellite */
    if (g.nav_mode == 2) return g.nav_sat <= 30 ? 2 : (g.nav_sat <= 60 ? 3 : 4);
    return 0;                                      /* road */
}

#define NAV_SPEED_GREEN RGB565(0x44, 0xff, 0x44)

static void nav_pill_shape(int x, int y, int w, int h, uint16_t c) {
    int r = h / 2;
    gfx_fill(x + r, y, w - 2 * r, h, c);
    gfx_disc(x + r, y + r, r, c);
    gfx_disc(x + w - r - 1, y + r, r, c);
}

static void nav_pill(int right, int y, const char *s) {
    int w = gfx_text_w(s, 0) + 12;
    int h = 13;
    int x = right - w;
    uint16_t white = RGB565(0xff, 0xff, 0xff);
    nav_pill_shape(x - 1, y - 1, w + 2, h + 2, 0x0000);
    nav_pill_shape(x, y, w, h, white);
    gfx_text(x + 6, y + 3, s, 0x0000, 0);
}

static void nav_info_line(bool full, int right, int y, const char *s, uint16_t color) {
    if (full) nav_pill(right, y, s);
    else gfx_text_right(right, y + 3, s, color, 0);
}

static void nav_apply_zoom(int dir) {
    nav_zoom_step(dir);
    g.nav_zoom = nav_zoom_level();
    app_save_settings();
    app_toast(dir > 0 ? "ZOOM IN" : "ZOOM OUT");
}

static void nav_apply_style(int i) {
    if (i == 0) g.nav_mode = 0;
    else if (i == 1) g.nav_mode = 1;
    else { g.nav_mode = 2; g.nav_sat = i == 2 ? 30 : (i == 3 ? 60 : 90); }
    nav_set_mode(g.nav_mode, g.nav_sat);
    app_save_settings();
}

static void draw_nav_menu(void) {
    if (!g.nav_menu) return;
    static const char *it_main[5] = {"REFRESH", "STYLE", "ZOOM IN", "ZOOM OUT", "FULLSCREEN"};
    static const char *it_style[5] = {"ROAD PIXEL", "SATELLITE", "BLEND 30%", "BLEND 60%", "BLEND 90%"};
    bool style = (g.nav_menu == 2);
    const char **items = style ? it_style : it_main;
    int n = 5;
    int bw = 176, rh = 18, top = 24;
    int bh = top + n * rh + 16;
    int bx = (CW - bw) / 2, by = (CH - bh) / 2;
    gfx_fill(bx, by, bw, bh, PAL.stage);
    gfx_rect(bx, by, bw, bh, PAL.cyan);
    gfx_text(bx + 8, by + 7, style ? "STYLE" : (g.nav_full ? "LOCATION FULL" : "LOCATION"), PAL.cyan, 1);
    for (int i = 0; i < n; i++) {
        int y = by + top + i * rh;
        if (i == g.nav_sel) gfx_fill(bx + 4, y - 2, bw - 8, rh - 2, PAL.seltint);
        gfx_text(bx + 12, y, items[i], i == g.nav_sel ? PAL.yellow : PAL.text, 1);
    }
    gfx_text(bx + 8, by + bh - 11, "UP/DN  PUSH OK", PAL.dim, 0);
}

static void render_nav(void) {
    gps_state_t *gp = gps_get();
    bool full = g.nav_full != 0;
    int map_w = full ? CW : NAV_IMG_SIZE;
    int map_h = full ? CH : NAV_IMG_SIZE;
    char b[32];

    /* map image (server-rendered, heading-up, centred on us) */
    if (!g.entered) {
        gfx_fill(0, 0, map_w, map_h, PAL.mapbg);
        gfx_rect(0, 0, map_w, map_h, PAL.mapborder);
        gfx_text_center(map_w / 2, map_h / 2 - 30, "LOCATION", PAL.cyan, 2);
        gfx_text_center(map_w / 2, map_h / 2, "PUSH TO START", PAL.yellow, 1);
    } else {
        int mw = 0, mh = 0;
        const uint16_t *img = nav_frame_lock(&mw, &mh);
        if (full) {
            gfx_fill(0, 0, CW, CH, PAL.mapbg);
            if (img && mw > 0) {
                if (mw >= CW && mh >= CH) gfx_blit(0, 0, img, CW, CH, mw);
                else gfx_blit((CW - mw) / 2, (CH - mh) / 2, img, mw, mh, mw);
            }
        } else {
            gfx_fill(0, 0, NAV_IMG_SIZE, NAV_IMG_SIZE, PAL.mapbg);
            if (img && mw > 0) {
                int ox = (NAV_IMG_SIZE - mw) / 2;
                int oy = (NAV_IMG_SIZE - mh) / 2;
                gfx_blit(ox, oy, img, mw, mh, mw);
            }
        }
        nav_frame_unlock();
        if (!img) {
            gfx_text_center(map_w / 2, map_h / 2,
                            nav_status() == 4 ? "NO WIFI" : (nav_status() == 5 ? "NO GPS FIX" : "MAP LOADING..."),
                            PAL.dim, 1);
        }

        /* self arrow (heading-up -> always points up) */
        int cx = full ? CW / 2 : NAV_IMG_SIZE / 2;
        int cy = full ? CH / 2 : NAV_IMG_SIZE / 2;
        int ax[3] = { cx, cx - 9, cx + 9 };
        int ay[3] = { cy - 12, cy + 9, cy + 9 };
        gfx_poly_fill(ax, ay, 3, PAL.cyan);
        gfx_line(cx, cy - 12, cx - 9, cy + 9, PAL.white);
        gfx_line(cx - 9, cy + 9, cx + 9, cy + 9, PAL.white);
        gfx_line(cx + 9, cy + 9, cx, cy - 12, PAL.white);
    }

    /* speed: bright green, top-right */
    snprintf(b, sizeof b, "%d", (int)lroundf(gp->speed_kmh));
    int sw = gfx_text_w(b, 4);
    int sx = CW - 6 - sw;
    gfx_text(sx + 1, 7, b, 0x0000, 4);
    gfx_text(sx, 6, b, NAV_SPEED_GREEN, 4);
    gfx_text_right(CW - 6, 36, "KM/H", NAV_SPEED_GREEN, 1);

    /* secondary status: bottom-right, never overlapping the speed */
    char lines[3][24];
    uint16_t colors[3];
    int n = 0;

    if (gp->fix_type >= 1 && gp->has_pos) {
        snprintf(lines[n], sizeof lines[n], "%s %d", gp->fix_type == 2 ? "3D" : "2D", gp->sats_used);
        colors[n++] = PAL.green;
    } else {
        snprintf(lines[n], sizeof lines[n], "NO GPS");
        colors[n++] = PAL.red;
    }

    int st = nav_status();
    int age = nav_age_ms();
    if (st == 2) {
        if (nav_http_code()) snprintf(lines[n], sizeof lines[n], "HTTP %d", nav_http_code());
        else snprintf(lines[n], sizeof lines[n], "MAP ERR");
        colors[n++] = PAL.red;
    } else if (st == 3) {
        snprintf(lines[n], sizeof lines[n], "DECODE ERR");
        colors[n++] = PAL.red;
    } else if (st == 4) {
        snprintf(lines[n], sizeof lines[n], "NO WIFI");
        colors[n++] = PAL.red;
    } else if (st != 5) {
        if (st == 1 && age >= 0) {
            if (age < 1000) snprintf(lines[n], sizeof lines[n], "MAP %dms", age);
            else if (age < 10000) snprintf(lines[n], sizeof lines[n], "MAP %d.%ds", age / 1000, (age % 1000) / 100);
            else snprintf(lines[n], sizeof lines[n], "MAP %ds", age / 1000);
            colors[n++] = age < 3000 ? PAL.green : (age < 6000 ? PAL.yellow : PAL.red);
        } else {
            snprintf(lines[n], sizeof lines[n], "MAP...");
            colors[n++] = PAL.dim;
        }
    }

    if (g.entered) {
        snprintf(lines[n], sizeof lines[n], "PUSH MENU");
        colors[n++] = PAL.dim;
    }

    int right = CW - 5;
    int y = CH - 18 - 13;
    for (int i = n - 1; i >= 0; i--) {
        nav_info_line(full, right, y, lines[i], colors[i]);
        y -= 17;
    }

    draw_nav_menu();
}

/* ================= WIFI SETUP (SoftAP + phone web page) ================= */
static void render_wifi_setup(void) {
    char st[48];
    netcfg_status(st, sizeof st);
    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text(6, 2, "WIFI SETUP", PAL.cyan, 1);

    gfx_text(6, 22, "1) phone WiFi ->", PAL.dim, 0);
    gfx_text(6, 34, netcfg_ap_ssid(), PAL.white, 1);
    gfx_text(6, 54, "   pass: racebrick", PAL.dim, 0);
    gfx_text(6, 74, "2) browser open:", PAL.dim, 0);
    gfx_text(6, 86, "http://192.168.4.1", PAL.cyan, 1);
    gfx_text(6, 106, "3) enter home SSID / password", PAL.dim, 0);

    gfx_pxbox(6, 150, CW - 12, 40, PAL.line, 1, PAL.panel);
    gfx_text(12, 157, "STATUS", PAL.dim, 0);
    gfx_text(12, 172, st, netcfg_connected() ? PAL.green : PAL.yellow, 1);

    gfx_text(6, HINT_Y, "PUSH RESTART AP - BACK EXIT", PAL.dim, 0);
}

/* ================= TRACK EDIT ================= */
static void render_track_edit(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("TRACK EDIT", 6);
    hint_c("EDIT ON PHONE APP", 20);
    int y = 36;
    for (int i = 0; i < g.ntracks && i < 4; i++) {
        bool on = g.entered && i == g.sel;
        panel_box(12, y, CW - 24, 36, on, false);
        gfx_text(22, y + 5, g.tracks[i].name, PAL.white, 1);
        gfx_text(22, y + 20, VEH_NAMES[g.tracks[i].vehicle], PAL.dim, 1);
        y += 40;
    }
    if (!g.ntracks) hint_c("(NO TRACKS)", 60);
    hint_c(g.entered ? "PUSH VIEW - BACK EXIT" : "PUSH EDIT - UP/DOWN PAGE", HINT_Y);
}

/* ================= SETTINGS ================= */
static const char *settings_key(int i) {
    static const char *k[17] = {"BRIGHTNESS", "LED COLOR", "LED BRIGHT", "THEME", "WIFI SETUP", "BLUETOOTH", "SOUND", "GPS RATE", "LAUNCH G", "IMU DEBUG", "GPS DEBUG", "CHINA MAP", "UI STYLE", "BTN DEBUG", "FLASH MAP", "COVER TEST", "MEDIA COLOR"};
    return k[i];
}
static void settings_val(int i, char *out) {
    switch (i) {
    case 0: snprintf(out, 16, "%d%%", g.set_brightness); break;
    case 1: strcpy(out, LED_COLOR_NAMES[g.set_ledcolor]); break;
    case 2: snprintf(out, 16, "%d%%", g.set_ledbright); break;
    case 3: strcpy(out, g.theme_light ? "LIGHT" : "DARK"); break;
    case 5: strcpy(out, g.set_bt ? "ON" : "OFF"); break;
    case 7: snprintf(out, 16, "%dHz", g.gps_rate_hz); break;
    case 8: snprintf(out, 16, "0.%02dG", g.perf_trig_g100); break;
    case 12: strcpy(out, g.ui_style ? "FORMULA 1" : "PIXEL"); break;
    case 13: strcpy(out, ">"); break;
    default: strcpy(out, ">"); break;
    }
}
static void render_settings(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SETTINGS", 2);
    const int K = 6, h = 32;
    int start, end;
    windowed(17, g.sel, K, &start, &end);
    int y = 20;
    for (int i = start; i < end; i++) {
        bool on = g.entered && i == g.sel;
        panel_box(10, y, CW - 20, h - 4, on, false);
        gfx_text(20, y + 7, settings_key(i), PAL.text, 2);
        char v[16]; settings_val(i, v);
        bool editing = g.entered && g.set_edit && i == g.sel;
        gfx_text_right(CW - 18, y + 7, v, editing ? PAL.yellow : PAL.cyan, 2);
        y += h;
    }
    /* scroll indicators */
    if (start > 0) gfx_text(CW - 10, 4, "^", PAL.dim, 1);
    if (end < 17) gfx_text(CW - 10, 20 + K * h - 14, "v", PAL.dim, 1);
    hint_c("UP/DN SELECT - L/R ADJUST - PUSH ACTION", HINT_Y);
}

/* ================= FLASH MAP ================= */
typedef struct { uint32_t bytes, used; uint16_t col; } flash_seg_t;

static void flash_size(char *out, int n, uint32_t b) {
    if (b >= (1u << 20)) {
        uint32_t t = (uint32_t)(((uint64_t)b * 10u + (1u << 19)) >> 20);
        if (t % 10) snprintf(out, n, "%u.%uM", (unsigned)(t / 10), (unsigned)(t % 10));
        else        snprintf(out, n, "%uM", (unsigned)(t / 10));
    } else {
        snprintf(out, n, "%uK", (unsigned)((b + 1023) / 1024));
    }
}

static void flash_bar(int y, int h, uint32_t total, const flash_seg_t *s, int n, bool show_used) {
    int x = 10, w = CW - 20;
    gfx_fill(x, y, w, h, PAL.surface2);
    uint32_t acc = 0;
    int px = x;
    for (int i = 0; i < n; i++) {
        acc += s[i].bytes;
        int nx = (i == n - 1) ? x + w : x + (int)((uint64_t)acc * w / total);
        int sw = nx - px;
        if (sw < 0) sw = 0;
        if (show_used) {
            gfx_fill(px, y, sw, 2, s[i].col);
            int uw = 0;
            if (s[i].bytes && s[i].used)
                uw = (int)((uint64_t)sw * s[i].used / s[i].bytes);
            if (s[i].used && uw < 1 && sw > 0) uw = 1;
            if (uw > sw) uw = sw;
            if (uw > 0) gfx_fill(px, y + 2, uw, h - 2, s[i].col);
        } else {
            gfx_fill(px, y, sw, h, s[i].col);
        }
        px = nx;
    }
    gfx_rect(x, y, w, h, PAL.line);
}

static void flash_leg(int x, int y, uint16_t col, const char *label, const char *right) {
    gfx_fill(x, y + 2, 7, 7, col);
    gfx_text(x + 12, y, label, PAL.text, 0);
    if (right && *right) gfx_text_right(310, y, right, PAL.dim, 0);
}

static void render_flash(void) {
    char b[40], a[16], c[16];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("FLASH USAGE", 2);

    uint32_t itotal = 16u << 20;
    uint32_t phys = 0;
    if (esp_flash_get_physical_size(NULL, &phys) == ESP_OK && phys > 0) itotal = phys;
    const esp_partition_t *pa = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                                         ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    const esp_partition_t *pd = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                         ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    uint32_t app_b = pa ? pa->size : 0x300000u;
    uint32_t dat_b = pd ? pd->size : 0x200000u;
    uint32_t sys_b = 0x10000u;
    uint32_t iused = sys_b + app_b + dat_b;
    uint32_t ifree = itotal > iused ? itotal - iused : 0;

    flash_seg_t iseg[4] = {
        {sys_b, 0, PAL.dim},
        {app_b, 0, PAL.cyan},
        {dat_b, 0, PAL.green},
        {ifree, 0, PAL.surface2},
    };

    snprintf(b, sizeof b, "ESP32-S3  %uMB", (unsigned)(itotal >> 20));
    gfx_text(10, 20, b, PAL.cyan, 1);
    flash_size(a, sizeof a, iused);
    snprintf(b, sizeof b, "ALLOC %s", a);
    gfx_text_right(310, 20, b, PAL.dim, 0);
    flash_bar(34, 16, itotal, iseg, 4, false);
    flash_leg(10, 54, PAL.dim, "BOOT/NVS 64K", NULL);
    flash_size(a, sizeof a, app_b);
    snprintf(b, sizeof b, "APP %s", a);
    flash_leg(160, 54, PAL.cyan, b, NULL);
    flash_size(a, sizeof a, dat_b);
    snprintf(b, sizeof b, "DATA %s", a);
    flash_leg(10, 68, PAL.green, b, NULL);
    flash_size(a, sizeof a, ifree);
    snprintf(b, sizeof b, "FREE %s", a);
    flash_leg(160, 68, PAL.line, b, NULL);

    uint32_t track_total = (uint32_t)TSTORE_EXT_SLOTS * 4096u;
    uint32_t sess_total = (uint32_t)RACE_SLOTS * RACE_SLOT_SIZE;
    uint32_t log_total = log_store_total_bytes();
    uint32_t etotal = track_total + sess_total + log_total;
    uint32_t track_used = (uint32_t)tstore_count() * 4096u;
    uint32_t sess_used = (uint32_t)race_store_count() * RACE_SLOT_SIZE;
    uint32_t log_used = log_store_used_bytes();
    uint32_t eused = track_used + sess_used + log_used;
    flash_seg_t eseg[3] = {
        {track_total, track_used, PAL.green},
        {sess_total, sess_used, PAL.cyan},
        {log_total, log_used, PAL.purple},
    };

    gfx_text(10, 84, "EXTERNAL W25Q128  16MB", PAL.cyan, 1);
    flash_size(a, sizeof a, eused);
    snprintf(b, sizeof b, "USED %s", a);
    gfx_text_right(310, 84, b, PAL.dim, 0);
    flash_bar(96, 18, etotal, eseg, 3, true);

    flash_size(a, sizeof a, track_total);
    snprintf(b, sizeof b, "TRACK %s", a);
    snprintf(c, sizeof c, "%d/%d", tstore_count(), tstore_capacity());
    flash_leg(10, 116, PAL.green, b, c);
    flash_size(a, sizeof a, sess_total);
    snprintf(b, sizeof b, "SESSION %s", a);
    snprintf(c, sizeof c, "%d/%d", race_store_count(), RACE_SLOTS);
    flash_leg(10, 132, PAL.cyan, b, c);
    flash_size(a, sizeof a, log_total);
    snprintf(b, sizeof b, "RACE LOG %s", a);
    snprintf(c, sizeof c, "%d/%d", log_store_count(), log_store_capacity());
    flash_leg(10, 148, PAL.purple, b, c);

    flash_size(a, sizeof a, eused);
    flash_size(c, sizeof c, etotal);
    snprintf(b, sizeof b, "USED %s / %s", a, c);
    gfx_text(10, 164, b, PAL.white, 1);
    gfx_text(10, 180, "EXT: OUTLINE=CAPACITY  FILL=USED", PAL.dim, 0);
    hint_c("BACK EXIT", HINT_Y);
}

/* ================= SOUND ================= */
static void render_sound(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SOUND", 2);
    static const char *keys[5] = {"VOLUME", "CONFIRM", "CALIB", "3D FIX", "MAX DUDUDU"};
    const int h = 34;
    int y = 26;
    for (int i = 0; i < 5; i++) {
        bool on = i == g.sel;
        panel_box(10, y, CW - 20, h - 4, on, false);
        gfx_text(20, y + 7, keys[i], PAL.text, 2);
        char v[16];
        if (i == 0) snprintf(v, sizeof v, "%d%%", g.set_sound_vol);
        else if (i == 4) strcpy(v, "PLAY");
        else {
            bool b = i == 1 ? g.set_snd_confirm : (i == 2 ? g.set_snd_calib : g.set_snd_fix);
            strcpy(v, b ? "ON" : "OFF");
        }
        gfx_text_right(CW - 18, y + 7, v, PAL.cyan, 2);
        y += h;
    }
    hint_c("UP/DN SELECT - L/R VOLUME - PUSH ACTION - BACK", HINT_Y);
}

/* ================= IMU DEBUG (real MPU6050) ================= */
static void dbg_bar(int y, const char *label, float v, float half, uint16_t col) {
    gfx_text(160, y, label, PAL.dim, 0);
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
    int cx = 84, cy = 128, R = 68;
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
    gfx_text(160, 34, "G FORCE", PAL.cyan, 0);
    dbg_bar(48, "GX", g.i_ax, 2.0f, PAL.green);
    dbg_bar(68, "GY", g.i_ay, 2.0f, PAL.green);
    dbg_bar(88, "GZ", g.i_az, 2.0f, PAL.cyan);
    /* rotation rate */
    gfx_text(160, 108, "RATE d/s", PAL.cyan, 0);
    dbg_bar(122, "RX", g.i_gx, 250.0f, PAL.yellow);
    dbg_bar(142, "RY", g.i_gy, 250.0f, PAL.yellow);
    dbg_bar(162, "RZ", g.i_gz, 250.0f, PAL.orange);

    char t[16];
    {
        float a3[3] = {g.i_ax, g.i_ay, g.i_az};
        float h1 = a3[(g.i_vaxis + 1) % 3], h2 = a3[(g.i_vaxis + 2) % 3];
        u_fmt_f(t, sizeof t, sqrtf(h1 * h1 + h2 * h2), 2);
    }
    gfx_text(160, 180, "|G|", PAL.dim, 0);
    gfx_text(182, 180, t, PAL.white, 0);
    u_fmt_f(t, sizeof t, g.i_gmax, 2);
    gfx_text(228, 180, "PEAK", PAL.dim, 0);
    gfx_text(256, 180, t, PAL.gold, 0);

    u_fmt_f(t, sizeof t, g.i_temp, 1);
    gfx_text(160, 196, "TEMP", PAL.dim, 0);
    gfx_text(190, 196, t, PAL.text, 0);
    gfx_text(224, 196, "C", PAL.dim, 0);

    gfx_text(6, 216, "PUSH TARE - BACK EXIT", PAL.dim, 0);
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
    mode_box(52, "NEW TRACK", "MAP + RACE MODE", g.sel == 0);
    mode_box(128, "RECORDED TRACK", "KNOWN TRACK", g.sel == 1);
    hint_c("UP/DOWN SELECT - PUSH ENTER - BACK", HINT_Y);
}

/* ================= MOUNT SELECT ================= */
static void render_mount_select(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("MOUNTING", 6);
    mode_box(52, "FIXED (CHASSIS)", "RIGID - TRUST IMU", g.sel == 0);
    mode_box(128, "NOT FIXED (WHEEL)", "ROTATES - TRUST GPS", g.sel == 1);
    hint_c("UP/DOWN SELECT - PUSH START - BACK", HINT_Y);
}

/* ================= NEW TRACK - MARK START ================= */
static int64_t rec_gnss_delta(uint64_t a, uint64_t b) {
    int64_t d = (int64_t)b - (int64_t)a;
    if (d < -43200000000LL) d += 86400000000LL;
    else if (d > 43200000000LL) d -= 86400000000LL;
    return d;
}

static void rec_draw_arrow(int cx, int cy, float hdg, uint16_t color) {
    /* hdg: 0 = north/up, positive clockwise; screen y grows down. */
    float fx = sinf(hdg), fy = -cosf(hdg);
    float rx = cosf(hdg), ry = sinf(hdg);
    int tipx = cx + (int)(fx * 11), tipy = cy + (int)(fy * 11);
    int lx = cx - (int)(fx * 7) + (int)(rx * 7);
    int ly = cy - (int)(fy * 7) + (int)(ry * 7);
    int rx2 = cx - (int)(fx * 7) - (int)(rx * 7);
    int ry2 = cy - (int)(fy * 7) - (int)(ry * 7);
    int nx = cx - (int)(fx * 3), ny = cy - (int)(fy * 3);
    int ax[4] = {tipx, lx, nx, rx2};
    int ay[4] = {tipy, ly, ny, ry2};
    gfx_poly_fill(ax, ay, 4, color);
    gfx_line(tipx, tipy, lx, ly, PAL.white);
    gfx_line(tipx, tipy, rx2, ry2, PAL.white);
}

static void rec_capture_lap(void) {
    if (!g.rec_lap_data || g.rec_lap_n >= REC_FUSION_LAPS) return;
    if (!g.sessionTrack_valid || g.sessionTrack.nscale <= 1e-9f) return;
    reclap_t *l = &g.rec_lap_data[g.rec_lap_n];
    l->time = g.rec_lap_time;
    for (int i = 0; i < TRK_N; i++) {
        l->points[i].x = (g.sessionTrack.points[i].x - g.sessionTrack.nox) / g.sessionTrack.nscale;
        l->points[i].y = (g.sessionTrack.points[i].y - g.sessionTrack.noy) / g.sessionTrack.nscale;
    }
    l->valid = true;
    g.rec_lap_n++;
}

static int rec_selected_count(void) {
    int n = 0;
    for (int i = 0; i < g.rec_lap_n; i++) if (g.rec_lap_sel & (1u << i)) n++;
    return n;
}

static bool rec_build_fusion(void) {
    int nsel = rec_selected_count();
    if (!nsel) return false;
    static pt_t avg[TRK_N];
    memset(avg, 0, sizeof(avg));
    for (int i = 0; i < g.rec_lap_n; i++) {
        if (!(g.rec_lap_sel & (1u << i))) continue;
        for (int k = 0; k < TRK_N; k++) {
            avg[k].x += g.rec_lap_data[i].points[k].x;
            avg[k].y += g.rec_lap_data[i].points[k].y;
        }
    }
    for (int k = 0; k < TRK_N; k++) {
        avg[k].x /= (float)nsel;
        avg[k].y /= (float)nsel;
    }
    track_build_geo(&g.sessionTrack, avg, TRK_N, g.rec_lat0, g.rec_lon0, "NEW", 0);
    g.sessionTrack_valid = true;
    return true;
}

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
    int mx = 4, my = 36, mw = 184, mh = 160;
    gfx_fill(mx, my, mw, mh, PAL.mapbg);
    gfx_rect(mx, my, mw, mh, PAL.mapborder);
    if (g.rec_n > 1 && g.rec_pts) {
        /* Keep the current position at the centre, and fit the complete
         * recorded trail around it. The view zooms out as the trail grows. */
        pt_t cur = g.rec_last;
        /* Heading-up camera: the arrow stays fixed at screen centre; the
         * world trail is rotated by the smoothed current heading. */
        float target_hdg = g.rec_disp_hdg;
        if (g.rec_n >= 2) {
            float dx = g.rec_pts[g.rec_n - 1].x - g.rec_pts[g.rec_n - 2].x;
            float dy = g.rec_pts[g.rec_n - 1].y - g.rec_pts[g.rec_n - 2].y;
            if (hypotf(dx, dy) > 0.4f) target_hdg = atan2f(dx, dy);
        }
        float dh = target_hdg - g.rec_disp_hdg;
        while (dh > (float)M_PI) dh -= 2.0f * (float)M_PI;
        while (dh < -(float)M_PI) dh += 2.0f * (float)M_PI;
        g.rec_disp_hdg += dh * 0.28f;
        float ch = cosf(g.rec_disp_hdg), sh = sinf(g.rec_disp_hdg);
        float maxdx = 8.0f, maxdy = 8.0f;
        for (int i = 0; i < g.rec_n; i++) {
            float dx = g.rec_pts[i].x - cur.x;
            float dy = g.rec_pts[i].y - cur.y;
            float rx = dx * ch - dy * sh;
            float ry = -(dx * sh + dy * ch);
            if (fabsf(rx) > maxdx) maxdx = fabsf(rx);
            if (fabsf(ry) > maxdy) maxdy = fabsf(ry);
        }
        float s = (mw - 18) / (2.0f * maxdx);
        float sy = (mh - 18) / (2.0f * maxdy);
        if (sy < s) s = sy;
        float cx = mx + mw / 2.0f, cy = my + mh / 2.0f;
        int stride = g.rec_n / 900;
        if (stride < 1) stride = 1;
        float dx0 = g.rec_pts[0].x - cur.x, dy0 = g.rec_pts[0].y - cur.y;
        float rx0 = dx0 * ch - dy0 * sh, ry0 = -(dx0 * sh + dy0 * ch);
        int px = (int)lroundf(cx + rx0 * s);
        int py = (int)lroundf(cy + ry0 * s);
        for (int i = 1; i < g.rec_n; i += stride) {
            float dx = g.rec_pts[i].x - cur.x, dy = g.rec_pts[i].y - cur.y;
            float rx = dx * ch - dy * sh, ry = -(dx * sh + dy * ch);
            int x = (int)lroundf(cx + rx * s);
            int y = (int)lroundf(cy + ry * s);
            gfx_line(px, py, x, y, PAL.gpstrace);
            px = x; py = y;
        }
        /* start/finish marker in the rotated view */
        float sxw = -cur.x, syw = -cur.y;
        float sxr = sxw * ch - syw * sh, syr = -(sxw * sh + syw * ch);
        int sx0 = (int)lroundf(cx + sxr * s);
        int sy0 = (int)lroundf(cy + syr * s);
        gfx_fill(sx0 - 2, sy0 - 2, 5, 5, PAL.white);
        gfx_fill(sx0 - 1, sy0 - 1, 3, 3, PAL.red);

        rec_draw_arrow((int)cx, (int)cy, 0.0f, PAL.red);
    } else {
        gfx_text(mx + 8, my + 8, g.rec_state == 1 ? "recording..." : "at the start line", PAL.dim, 0);
    }

    /* stats + hint */
    int rx = 196;
    if (g.rec_state == 1) {
        char tb[24];
        snprintf(b, sizeof b, "LAP %d", g.rec_laps + 1);
        gfx_text(rx, 36, b, PAL.cyan, 1);
        u_fmt_time(tb, sizeof tb, g.rec_lap_time);
        gfx_text(rx, 54, tb, PAL.white, 1);
        if (g.rec_laps > 0) {
            u_fmt_time(tb, sizeof tb, g.rec_last_lap);
            gfx_text(rx, 78, "LAST", PAL.dim, 0);
            gfx_text(rx + 32, 78, tb, PAL.text, 0);
            u_fmt_time(tb, sizeof tb, g.rec_best_lap);
            gfx_text(rx, 94, "BEST", PAL.dim, 0);
            gfx_text(rx + 32, 94, tb, PAL.gold, 0);
            char db[16];
            u_fmt_delta(db, sizeof db, g.rec_delta, 3);
            gfx_text(rx, 112, db, g.rec_delta <= 0.0f ? PAL.green : PAL.red, 1);
        } else {
            snprintf(b, sizeof b, "%.0f M", g.rec_dist);
            gfx_text(rx, 78, "DIST", PAL.dim, 0);
            gfx_text(rx + 32, 78, b, PAL.text, 0);
        }
    } else {
        snprintf(b, sizeof b, "%.0f M", g.rec_dist);
        gfx_text(rx, 40, "DIST", PAL.dim, 0);
        gfx_text(rx, 50, b, PAL.white, 1);
        snprintf(b, sizeof b, "%d", g.rec_n);
        gfx_text(rx, 74, "PTS", PAL.dim, 0);
        gfx_text(rx, 84, b, PAL.white, 1);
    }

    const char *hint;
    uint16_t hc;
    if (g.rec_state == 1) { hint = "PUSH FINISH"; hc = PAL.yellow; }
    else if (gp->fix_type >= 2) { hint = "PUSH START"; hc = PAL.cyan; }
    else { hint = "WAIT 3D FIX"; hc = PAL.orange; }
    gfx_text(rx, 112, hint, hc, 1);
    if (gp->fix_type < 2)
        gfx_text(6, 210, "DOWN: GPS DEBUG   BACK CANCEL", PAL.orange, 0);
    else
        gfx_text(6, 210, "PUSH START/FINISH   BACK CANCEL", PAL.dim, 0);
}

/* ================= RECORDED LAPS / FUSION ================= */
static pt_t rec_norm_point(pt_t m) {
    pt_t q = { m.x * g.sessionTrack.nscale + g.sessionTrack.nox,
               m.y * g.sessionTrack.nscale + g.sessionTrack.noy };
    return q;
}

static void render_rec_lap_select(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("RECORDED LAPS", 4);
    if (!s_map_ready) {
        map_init(&s_map, 4, 20, 176, 194);
        s_map_ready = true;
    }
    map_begin(&s_map);
    static pt_t tmp[TRK_N];
    int first = -1;
    for (int i = 0; i < g.rec_lap_n; i++) {
        if (g.rec_lap_sel & (1u << i)) { first = i; break; }
    }
    if (first < 0 && g.rec_lap_n) first = 0;
    if (first >= 0) {
        for (int i = 0; i < TRK_N; i++) tmp[i] = rec_norm_point(g.rec_lap_data[first].points[i]);
        map_fit_points(&s_map, tmp, TRK_N, 0.2f);
        for (int l = 0; l < g.rec_lap_n; l++) {
            if (!(g.rec_lap_sel & (1u << l))) continue;
            for (int i = 0; i < TRK_N; i++) tmp[i] = rec_norm_point(g.rec_lap_data[l].points[i]);
            map_trace(&s_map, tmp, TRK_N, l == first ? PAL.curtrace : PAL.histtrace, 2);
        }
    }

    int start, end;
    windowed(g.rec_lap_n + 1, g.rec_sel_cursor, 5, &start, &end);
    int x = 190, y = 30, rh = 28;
    for (int i = start; i < end; i++) {
        bool cur = i == g.rec_sel_cursor;
        if (i < g.rec_lap_n) {
            bool on = (g.rec_lap_sel & (1u << i)) != 0;
            gfx_pxbox(x, y, CW - x - 6, rh - 4, cur ? PAL.white : PAL.line, 1,
                      cur ? PAL.seltint : PAL.surface);
            gfx_fill(x + 6, y + 7, 9, 9, on ? PAL.green : PAL.checkboxoff);
            char b[24]; snprintf(b, sizeof b, "L%d", i + 1);
            gfx_text(x + 20, y + 4, b, PAL.white, 1);
            u_fmt_time(b, sizeof b, g.rec_lap_data[i].time);
            gfx_text_right(CW - 10, y + 5, b, PAL.text, 0);
        } else {
            gfx_pxbox(x, y, CW - x - 6, rh - 4, cur ? PAL.cyan : PAL.line, 1,
                      cur ? PAL.seltint : PAL.panel);
            gfx_text(x + 12, y + 4, "BUILD TRACK", PAL.cyan, 1);
            gfx_text_right(CW - 10, y + 5, ">", PAL.dim, 0);
        }
        y += rh;
    }
    char b[32]; snprintf(b, sizeof b, "%d LAPS SELECTED", rec_selected_count());
    gfx_text(6, 220, b, PAL.dim, 0);
    gfx_text_center(CW / 2, 232, "UP/DN SELECT  PUSH TOGGLE/BUILD  BACK", PAL.dim, 0);
}

static void render_rec_preview(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("FUSED TRACK", 4);
    if (!s_map_ready) {
        map_init(&s_map, 4, 22, CW - 8, 190);
        map_set_track(&s_map, &g.sessionTrack);
        s_map_ready = true;
    }
    map_begin(&s_map);
    map_track_ribbon(&s_map, TRACK_WIDTH_M, PAL.line2);
    map_centerline(&s_map, PAL.dim, 3);
    pt_t sp = track_point_at(&g.sessionTrack, 0);
    pt_t st = track_tangent_at(&g.sessionTrack, 0);
    map_start_checker_at(&s_map, sp, st, RGB565(0, 0, 0));
    gfx_text_center(CW / 2, 216, "PUSH CONFIRM - BACK REDO", PAL.dim, 0);
}

/* ================= RACING ================= */
static void render_panel_dots(void) {
    for (int i = 0; i < 4; i++) {
        int x = CW - 6 - (4 - i) * 9;
        gfx_fill(x, CH - 10, 5, 5, i == g.panel ? PAL.cyan : PAL.dotoff);
    }
}

static float track_line_fraction(const track_t *t) {
    if (!t || !t->geo || t->nscale <= 1e-9f) return 0.0f;
    geo_ref_t r = { t->glat0, t->glon0 };
    float mx, my;
    geo_meters(&r, t->line_lat, t->line_lon, &mx, &my);
    pt_t q = { mx * t->nscale + t->nox, my * t->nscale + t->noy };
    float s = 0, dist = 0;
    if (!track_project(t, q, &s, &dist)) return 0.0f;
    return s / (float)t->N;
}

static void racing_map(bool recorded, engine_t *e, float dt) {
    if (!s_map_ready) {
        if (recorded) map_init(&s_map, 4, 100, 180, 134);
        else map_init(&s_map, 0, 0, 150, 226);
        if (recorded && e->track) map_set_track(&s_map, e->track);
        s_map_ready = true;
    }
    map_begin(&s_map);
    if (recorded) {
        map_track_ribbon(&s_map, TRACK_WIDTH_M, PAL.line2);
        map_centerline(&s_map, PAL.dim, 3);
    } else {
        map_fit_engine(&s_map, e, 0.28f);
        map_smooth(&s_map, dt, 4);
    }
    for (int l = 0; l < e->nLaps; l++)
        map_trace(&s_map, e->laps[l].gps, e->laps[l].nGps, PAL.gpstracehist, 2);
    map_trace(&s_map, e->gpsLapTrace, e->nGps, PAL.gpstrace, 2);
    map_trail(&s_map, e);
    if (recorded) {
        float sf = track_line_fraction(e->track);
        pt_t sp = track_point_at(e->track, sf);
        pt_t st = track_tangent_at(e->track, sf);
        map_start_checker_at(&s_map, sp, st, RGB565(0, 0, 0));
        for (int i = 0; i < e->track->nsec; i++) map_tick(&s_map, e->track->sections[i], PAL.orange, 4);
        float f = engine_progress(e);
        pt_t cp = track_point_at(e->track, f);
        pt_t ct = track_tangent_at(e->track, f);
        if (e->track->reverse) { ct.x = -ct.x; ct.y = -ct.y; }
        map_start_arrow_at(&s_map, cp, ct, PAL.red);
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
        if (recorded) {
            /* Top half: large live time + delta. Bottom-left: compact track.
             * Bottom-right: recent laps and best lap. */
            gfx_text(10, 4, "LIVE", PAL.dim, 1);
            if (e->outlap_pending) snprintf(b2, sizeof b2, "OUTLAP");
            else snprintf(b2, sizeof b2, "LAP %d", e->lap);
            gfx_text_right(CW - 10, 4, b2, PAL.dim, 1);
            u_fmt_time(b, sizeof b, e->elapsed);
            gfx_text_center(CW / 2, 20, b, PAL.white, 5);
            u_fmt_delta(b2, sizeof b2, e->diff, 3);
            uint16_t dc = fabsf(e->diff) < 0.02f ? PAL.dim : (e->diff > 0 ? PAL.red : PAL.green);
            gfx_text_center(CW / 2, 66, b2, dc, 4);

            int lx = 190, ly = 100, lw = CW - lx - 8, rh = 32;
            int row = 0;
            if (e->hasBestLap) {
                int y = ly;
                gfx_fill(lx, y, lw, rh - 2, PAL.purple);
                snprintf(b2, sizeof b2, "BEST L%d", e->bestIndex);
                gfx_text(lx + 6, y + 3, b2, PAL.white, 1);
                u_fmt_time(b, sizeof b, e->bestTime);
                gfx_text_right(CW - 12, y + 14, b, PAL.white, 2);
                row = 1;
            }
            int shown = e->nLaps < 3 ? e->nLaps : 3;
            for (int k = 0; k < shown; k++) {
                int idx = e->nLaps - 1 - k;
                lap_t *lap = &e->laps[idx];
                int y = ly + (row + k) * rh;
                bool best = e->hasBestLap && lap->index == e->bestIndex;
                gfx_fill(lx, y, lw, rh - 2, best ? PAL.purple : PAL.surface);
                snprintf(b2, sizeof b2, "L%d", lap->index);
                gfx_text(lx + 6, y + 3, b2, best ? PAL.white : PAL.dim, 1);
                u_fmt_time(b, sizeof b, lap->time);
                gfx_text_right(CW - 12, y + 14, b, best ? PAL.white : PAL.text2, 2);
            }
            if (!e->hasBestLap && shown == 0) gfx_text(lx + 6, ly + 6, "NO LAPS", PAL.dim, 1);
        } else {
            int rx = 156;
            if (f1_ui()) gfx_fill(rx, 0, CW - rx, 3, PAL.cyan);
            gfx_text(rx, 6, "LIVE", PAL.dim, 1);
            u_fmt_time(b, sizeof b, e->elapsed);
            gfx_text(rx, 20, b, PAL.white, 2);
            u_fmt_delta(b2, sizeof b2, e->diff, 3);
            uint16_t dc = fabsf(e->diff) < 0.02f ? PAL.dim : (e->diff > 0 ? PAL.red : PAL.green);
            gfx_text(rx, 44, b2, dc, 2);
            int shown = e->nLaps < 4 ? e->nLaps : 4;
            for (int k = 0; k < shown; k++) {
                int idx = e->nLaps - 1 - k;
                lap_t *lap = &e->laps[idx];
                int y = 72 + k * 22;
                bool best = e->hasBestLap && lap->index == e->bestIndex;
                if (best) gfx_fill(rx, y, CW - rx - 6, 20, PAL.purple);
                else gfx_fill(rx, y, CW - rx - 6, 20, f1_ui() && (lap->index & 1) ? PAL.surface2 : PAL.surface);
                if (f1_ui()) gfx_fill(rx, y, 2, 20, best ? PAL.purple : PAL.cyan);
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
        }
    } else if (g.panel == 1) {
        bool neutral = !isfinite((double)e->diff) || fabsf(e->diff) < 0.02f;
        uint16_t top_bg = neutral ? PAL.surface : (e->diff > 0 ? PAL.red : PAL.green);
        gfx_fill(0, 0, CW, CH, PAL.stage);
        gfx_fill(0, 0, CW, CH / 2, top_bg);

        /* Delta is the primary timing-tower value. */
        gfx_text(12, 5, "DELTA", PAL.white, 1);
        snprintf(b2, sizeof b2, e->outlap_pending ? "OUTLAP" : "LAP %d", e->lap);
        gfx_text_right(CW - 12, 5, b2, PAL.white, 1);
        if (neutral) snprintf(b, sizeof b, "--.---");
        else u_fmt_delta(b, sizeof b, e->diff, 3);
        gfx_text_center(CW / 2, 18, b, PAL.white, 5);

        /* Live lap time is the second line. */
        gfx_text(12, 60, "LIVE", PAL.white, 1);
        u_fmt_time(b, sizeof b, e->elapsed);
        gfx_text_center(CW / 2, 72, b, PAL.white, 4);

        /* Best lap below the coloured header. */
        gfx_text(12, 128, "BEST", PAL.gold, 1);
        if (e->hasBestLap) {
            snprintf(b2, sizeof b2, "L%d", e->bestIndex);
            gfx_text_right(CW - 12, 128, b2, PAL.gold, 1);
            u_fmt_time(b, sizeof b, e->bestTime);
            gfx_text_center(CW / 2, 144, b, PAL.white, 4);
        } else {
            gfx_text_center(CW / 2, 144, "--:--.---", PAL.dim, 4);
        }
    } else if (g.panel == 2) {
        gps_state_t *gp = gps_get();
        bool real = recorded && (gp->source == 0 || gp->source == 1);
        int sats_used = real ? gp->sats_used : g.gnss.used;
        int sats_vis = real ? gp->sats_visible : g.gnss.visible;
        float hdop = real ? gp->hdop : g.gnss.hdop;
        int fix = real ? gp->fix_type : g.gnss.fix;
        float alt = real ? gp->alt : engine_alt_m(e);
        int hdg = real && gp->has_pos ? (int)lroundf(gp->course) : (int)lroundf(engine_heading_deg(e));

        /* large G-G plot on the left, with absolute FWD/RIGHT/LEFT/REV
         * component values in its four corners */
        map_draw_gmeter(10, 46, 156, 148, e->gLat, e->gLong,
                        e->gTrail, e->nGTrail, 2);
        char gv[16];
        u_fmt_f(gv, sizeof gv, e->gLong > 0 ? e->gLong : 0, 2);
        gfx_text_center(88, 22, "FWD", PAL.dim, 0);
        gfx_text_center(88, 32, gv, PAL.white, 1);
        u_fmt_f(gv, sizeof gv, e->gLat > 0 ? e->gLat : 0, 2);
        gfx_text_right(170, 108, "RIGHT", PAL.dim, 0);
        gfx_text_right(170, 119, gv, PAL.white, 1);
        u_fmt_f(gv, sizeof gv, e->gLat < 0 ? -e->gLat : 0, 2);
        gfx_text(4, 108, "LEFT", PAL.dim, 0);
        gfx_text(4, 119, gv, PAL.white, 1);
        u_fmt_f(gv, sizeof gv, e->gLong < 0 ? -e->gLong : 0, 2);
        gfx_text_center(88, 202, "REV", PAL.dim, 0);
        gfx_text_center(88, 212, gv, PAL.white, 1);

        /* speed + distance, same information as before */
        int sx = (int)lroundf(engine_speed_kmh(e));
        snprintf(b, sizeof b, "%d", sx);
        gfx_text(188, 20, "SPEED", PAL.dim, 0);
        gfx_text(184, 28, b, PAL.white, 6);
        gfx_text(190, 84, "KM/H", PAL.dim, 0);
        snprintf(b, sizeof b, "%.0f M", engine_dist_m(e));
        gfx_text(190, 102, "DIST", PAL.dim, 0);
        gfx_text(190, 114, b, PAL.cyan, 1);

        /* real GNSS status in the lower-right */
        gfx_hline(186, 136, 124, PAL.line2);
        gfx_text(188, 142, "GNSS", PAL.cyan, 0);
        snprintf(b, sizeof b, "%d/%d", sats_used, sats_vis);
        gfx_text(188, 156, "SATS", PAL.dim, 0);
        gfx_text_right(310, 156, b, sats_used ? PAL.green : PAL.red, 0);
        u_fmt_f(b, sizeof b, hdop, 1);
        gfx_text(188, 170, "HDOP", PAL.dim, 0);
        gfx_text_right(310, 170, b, PAL.text, 0);
        gfx_text(188, 184, "FIX", PAL.dim, 0);
        gfx_text_right(310, 184, fix_str(fix), fix ? PAL.green : PAL.red, 0);
        u_fmt_f(b, sizeof b, alt, 1);
        gfx_text(188, 198, "ALT", PAL.dim, 0);
        gfx_text_right(310, 198, b, PAL.text, 0);
        snprintf(b, sizeof b, "%d", hdg);
        gfx_text(188, 212, "HDG", PAL.dim, 0);
        gfx_text_right(310, 212, b, PAL.text, 0);
        if (real) {
            snprintf(b, sizeof b, "%s", gps_pps_ok() ? "PPS" : "--");
            gfx_text(188, 226, "SYNC", PAL.dim, 0);
            gfx_text_right(310, 226, b, gps_pps_ok() ? PAL.green : PAL.dim, 0);
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
        hint_c("PUSH END RACE", HINT_Y);
    }
}

/* ================= LAP SELECT ================= */
static void render_lap_select(void) {
    char b[24];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    if (!s_map_ready) { map_init(&s_map, 0, 0, 176, 226); if (g.engine.track) map_set_track(&s_map, g.engine.track); s_map_ready = true; }
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
    gfx_text(182, 190, b, PAL.dim, 0);
    gfx_text(182, 206, "PUSH CHECK - 2xPUSH GO", PAL.dim, 0);
}

/* ================= SECTION MARKING ================= */
static void render_section(void) {
    char b[32];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    if (g.sessionTrack.N <= 0) { title_c("SECTION", 6); hint_c("NO ACTIVE TRACK", 100); return; }
    if (!s_map_ready) { map_init(&s_map, 0, 0, 176, 226); map_set_track(&s_map, &g.sessionTrack); s_map_ready = true; }
    map_begin(&s_map);
    map_track_ribbon(&s_map, TRACK_WIDTH_M, PAL.line2);
    map_centerline(&s_map, PAL.dim, 3);
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
    gfx_text(182, 168, "UP/DOWN MOVE", PAL.dim, 1);
    gfx_text(182, 184, "PUSH MARK", PAL.dim, 1);
    gfx_text(182, 200, "2xPUSH FINISH", PAL.dim, 1);
    gfx_text(182, 216, "BACK EXIT", PAL.dim, 1);
}

/* ================= VEHICLE SELECT ================= */
static void render_vehicle(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("SELECT VEHICLE", 6);
    for (int i = 0; i < 4; i++) {
        int cx = 160 + (i - g.sel) * 120;
        if (cx < -60 || cx > CW + 60) continue;
        bool on = i == g.sel;
        map_draw_vehicle(cx - 36, 64, 72, 56, on ? VEH_COLORS[i] : PAL.glyphoff, PAL.yellow);
        gfx_text_center(cx, 130, VEH_NAMES[i], on ? PAL.cyan : PAL.dim, 1);
    }
    hint_c("UP/DOWN SELECT - PUSH CONFIRM", HINT_Y);
}

/* ================= TRACK NAMING ================= */
static void render_naming(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("NAME TRACK", 6);
    char big[2] = {g.avail[g.sel], 0};
    gfx_text_center(CW / 2, 40, big, PAL.cyan, 8);
    int step = 8, total = g.navail * step;
    int x0 = (CW - total) / 2;
    for (int i = 0; i < g.navail; i++) {
        char ch[2] = {g.avail[i], 0};
        if (i == g.sel) {
            gfx_fill(x0 + i * step - 1, 156, 8, 14, PAL.cyan);
            gfx_text(x0 + i * step, 160, ch, PAL.white, 0);
        } else {
            gfx_text(x0 + i * step, 160, ch, PAL.glyphoff, 0);
        }
    }
    hint_c("UP/DOWN LETTER - PUSH SAVE", HINT_Y);
}

/* ================= TRACK LIST ================= */
static void render_track_list(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("TRACKS", 6);
    {
        char fs[32];
        int used = tstore_count(), cap = tstore_capacity();
        snprintf(fs, sizeof fs, "FLASH %d/%d  %dK FREE", used, cap, (cap - used) * 4);
        gfx_text_right(CW - 8, 6, fs, used < cap ? PAL.dim : PAL.red, 0);
    }
    if (!g.ntracks) hint_c("(NO TRACKS)", 60);
    int start, end;
    windowed(g.ntracks, g.cursor, 4, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 22 + (i - start) * 44;
        bool cur = i == g.cursor;
        gfx_pxbox(12, y, CW - 24, 40, cur ? PAL.white : PAL.line, 1, PAL.panel);
        map_draw_thumb(20, y + 6, 36, 28, &g.tracks[i], PAL.dim);
        gfx_text(70, y + 5, g.tracks[i].name, PAL.white, 1);
        gfx_text(70, y + 20, VEH_NAMES[g.tracks[i].vehicle], PAL.dim, 1);
    }
    hint_c("UP/DOWN SELECT - PUSH ENTER", HINT_Y);
}

/* ================= START LINE EDIT ================= */
static void render_track_start(void) {
    track_t *t = g.engine.track;
    if (!t) return;
    if (!s_map_ready) { map_init(&s_map, 0, 0, 184, 226); map_set_track(&s_map, t); s_map_ready = true; }
    map_begin(&s_map);
    map_track_ribbon(&s_map, TRACK_WIDTH_M, PAL.line2);
    map_centerline(&s_map, PAL.dim, 4);
    pt_t sp = track_point_at(t, g.start_f);
    pt_t st = track_tangent_at(t, g.start_f);
    if (t->reverse) { st.x = -st.x; st.y = -st.y; }
    map_start_checker_at(&s_map, sp, st, PAL.red);
    map_start_arrow_at(&s_map, sp, st, PAL.red);

    int rx = 190;
    char b[24];
    gfx_text(rx, 8, "START LINE", PAL.cyan, 1);
    gfx_text(rx, 28, "UP/DN DIR  L/R MOVE", PAL.dim, 0);
    u_fmt_f(b, sizeof b, g.start_f * 100.0f, 1);
    gfx_text(rx, 48, "POS", PAL.dim, 0);
    gfx_text(rx + 34, 48, b, PAL.white, 0);
    gfx_text(rx + 78, 48, "%", PAL.dim, 0);
    u_fmt_f(b, sizeof b, g.start_f * t->lengthM, 0);
    gfx_text(rx, 64, "OFF", PAL.dim, 0);
    gfx_text(rx + 34, 64, b, PAL.white, 0);
    gfx_text(rx + 80, 64, "M", PAL.dim, 0);
    gfx_text(rx, 88, t->reverse ? "DIR CCW" : "DIR CW",
             t->reverse ? PAL.orange : PAL.green, 1);
    gfx_text(rx, 120, "PUSH SAVE", PAL.green, 1);
    gfx_text(rx, 140, "BACK CANCEL", PAL.dim, 0);
}

/* ================= HISTORY ================= */
static void render_history(void) {
    char b[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    map_init(&s_map, 0, 0, 176, 226);
    map_begin(&s_map);
    if (g.nlog) {
        histrec_t *rec = &g.log[g.nlog - 1 - g.sel];
        map_fit_points(&s_map, rec->trace, rec->nTrace, 0.3f);
        map_trace(&s_map, rec->trace, rec->nTrace, PAL.purple, 2);
    }
    title_c("RACE LOG", 6);
    int start, end;
    snprintf(b, sizeof b, "LOG %d/%d", log_store_count(), log_store_capacity());
    gfx_text(182, 18, b, PAL.dim, 0);
    snprintf(b, sizeof b, "%uK/%uK", (unsigned)(log_store_used_bytes() / 1024),
             (unsigned)(log_store_total_bytes() / 1024));
    gfx_text_right(310, 18, b, PAL.dim, 0);
    windowed(g.nlog, g.sel, 4, &start, &end);
    for (int i = start; i < end; i++) {
        int y = 34 + (i - start) * 38;
        bool on = g.entered && i == g.sel;
        gfx_pxbox(182, y, 132, 36, on ? PAL.white : PAL.line, 1, PAL.panel);
        histrec_t *r = &g.log[g.nlog - 1 - i];
        snprintf(b, sizeof b, "%s  %dL", r->trackName, r->lapCount);
        gfx_text(188, y + 5, b, PAL.white, 1);
        snprintf(b, sizeof b, "%dK", log_store_slot_size() / 1024);
        gfx_text_right(310, y + 5, b, PAL.gold, 0);
        u_fmt_time(b, sizeof b, r->duration);
        gfx_text(188, y + 20, r->date, PAL.dim, 0);
        gfx_text_right(310, y + 20, b, PAL.dim, 0);
    }
    if (!g.nlog) hint_c("(NO RECORDS)", 60);
    gfx_text(182, 196, g.entered ? "PUSH DETAIL" : "PUSH ENTER", PAL.dim, 1);
    gfx_text(182, HINT_Y, "BACK EXIT", PAL.dim, 1);
}

/* ================= HISTORY DETAIL ================= */
static void render_history_detail(void) {
    char b[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    histrec_t *r = (g.hist_rec >= 0 && g.hist_rec < g.nlog) ? &g.log[g.hist_rec] : NULL;
    map_init(&s_map, 0, 0, 176, 226);
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
    windowed(r->lapCount, g.sel, 5, &start, &end);
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
    gfx_text(182, 158, "SECTION BEST", PAL.gold, 1);
    for (int i = 0; i < r->nsec && i < 3; i++) {
        int y = 172 + i * 16;
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
    if (gps_pps_ok()) {
        char pb[20]; snprintf(pb, sizeof pb, "PPS %dms", gps_pps_period_ms());
        gfx_text_right(CW - 8, 70, pb, PAL.green, 0);
    } else {
        gfx_text_right(CW - 8, 70, "PPS --", PAL.dim, 0);
    }

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
    gfx_pxbox(4, 142, CW - 8, CH - 162, PAL.line, 1, PAL.surface);
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

/* ================= PHONE (NavBridge BLE full-frame map) ================= */
static void render_phone(void) {
    char b[64];
    if (!g.entered) {
        gfx_fill(0, 0, CW, CH, PAL.stage);
        gfx_text_center(CW / 2, 54, "PHONE MAP", PAL.cyan, 2);
        gfx_text_center(CW / 2, 92, "NAVBRIDGE BLE", PAL.white, 1);
        gfx_text_center(CW / 2, 118, btmap_state(), btmap_connected() ? PAL.green : PAL.yellow, 1);
        snprintf(b, sizeof b, "FRAMES %d", btmap_frames());
        gfx_text_center(CW / 2, 138, b, PAL.dim, 0);
        gfx_text_center(CW / 2, 170, "PUSH TO VIEW", PAL.white, 1);
        gfx_text_center(CW / 2, 198, "OPEN NAVBRIDGE ON PHONE", PAL.dim, 0);
        gfx_text_center(CW / 2, 214, "L/R PAGE - BACK HOME", PAL.dim, 0);
        return;
    }

    int w = 0, h = 0;
    const uint16_t *img = btmap_frame_lock(&w, &h);
    if (img && w == CW && h == CH) {
        gfx_blit(0, 0, img, w, h, w);
        btmap_frame_unlock();
        return;
    }
    btmap_frame_unlock();

    gfx_fill(0, 0, CW, CH, PAL.stage);
    gfx_text_center(CW / 2, 78, "PHONE MAP", PAL.cyan, 2);
    gfx_text_center(CW / 2, 118, btmap_state(), btmap_connected() ? PAL.green : PAL.yellow, 1);
    gfx_text_center(CW / 2, 142, btmap_connected() ? "WAITING FOR FRAME..." : "CONNECT VIA NAVBRIDGE", PAL.dim, 1);
    if (btmap_has_frame()) {
        int age = btmap_age_ms();
        snprintf(b, sizeof b, "LAST %d MS", age < 0 ? 0 : age);
        gfx_text_center(CW / 2, 166, b, PAL.dim, 0);
    }
    gfx_text_center(CW / 2, 210, "BACK EXIT", PAL.dim, 0);
}

/* ================= MEDIA (phone Bluetooth playback control) ================= */
static int s_media_hit = -1;
static int64_t s_media_hit_us = 0;

static bool media_pressed(int idx) {
    return s_media_hit == idx && (esp_timer_get_time() - s_media_hit_us) < 220000;
}

static void media_circle(int cx, int cy, int r, bool pressed) {
    uint16_t fill = pressed ? PAL.seltint : PAL.panel;
    uint16_t ring = pressed ? PAL.yellow : PAL.cyan;
    gfx_disc(cx, cy, r, fill);
    gfx_circle(cx, cy, r, ring);
    gfx_circle(cx, cy, r - 1, ring);
}

static void media_prev_icon(int cx, int cy, uint16_t c) {
    int a1[3] = { cx - 18, cx - 5, cx - 5 };
    int b1[3] = { cy, cy - 13, cy + 13 };
    int a2[3] = { cx - 4, cx + 9, cx + 9 };
    int b2[3] = { cy, cy - 13, cy + 13 };
    gfx_poly_fill(a1, b1, 3, c);
    gfx_poly_fill(a2, b2, 3, c);
}

static void media_next_icon(int cx, int cy, uint16_t c) {
    int a1[3] = { cx + 18, cx + 5, cx + 5 };
    int b1[3] = { cy, cy - 13, cy + 13 };
    int a2[3] = { cx + 4, cx - 9, cx - 9 };
    int b2[3] = { cy, cy - 13, cy + 13 };
    gfx_poly_fill(a1, b1, 3, c);
    gfx_poly_fill(a2, b2, 3, c);
}

/* Flat wide chevrons for the glass transport bar. */
static void media_flat_prev(int cx, int cy, uint16_t c) {
    int height = 6;
    int a1[3] = {cx - 15, cx - 6, cx - 6};
    int b1[3] = {cy, cy - height, cy + height};
    int a2[3] = {cx - 4, cx + 5, cx + 5};
    int b2[3] = {cy, cy - height, cy + height};
    gfx_poly_fill(a1, b1, 3, c);
    gfx_poly_fill(a2, b2, 3, c);
}
static void media_flat_next(int cx, int cy, uint16_t c) {
    int height = 6;
    int a1[3] = {cx + 15, cx + 6, cx + 6};
    int b1[3] = {cy, cy - height, cy + height};
    int a2[3] = {cx + 4, cx - 5, cx - 5};
    int b2[3] = {cy, cy - height, cy + height};
    gfx_poly_fill(a1, b1, 3, c);
    gfx_poly_fill(a2, b2, 3, c);
}

static void media_flat_play(int cx, int cy, uint16_t c) {
    int ax[3] = {cx - 6, cx - 6, cx + 8};
    int ay[3] = {cy - 8, cy + 8, cy};
    gfx_poly_fill(ax, ay, 3, c);
}
static void media_flat_pause(int cx, int cy, uint16_t c) {
    gfx_fill(cx - 7, cy - 8, 5, 16, c);
    gfx_fill(cx + 2, cy - 8, 5, 16, c);
}

static void media_play_icon(int cx, int cy, uint16_t c) {
    int ax[3] = { cx - 8, cx - 8, cx + 17 };
    int ay[3] = { cy - 15, cy + 15, cy };
    gfx_poly_fill(ax, ay, 3, c);
}

static void media_pause_icon(int cx, int cy, uint16_t c) {
    gfx_fill(cx - 11, cy - 14, 8, 28, c);
    gfx_fill(cx + 3, cy - 14, 8, 28, c);
}

#define MEDIA_COVER_W 240
#define MEDIA_COVER_H 240
static uint16_t *s_cover_cache;
static uint32_t s_cover_seq;
static uint16_t s_cover_bg;

static inline uint16_t blend565(uint16_t fg, uint16_t bg, int a) { /* a: 0..256 */
    int fr = (fg >> 11) & 31, fgc = (fg >> 5) & 63, fb = fg & 31;
    int br = (bg >> 11) & 31, bgc = (bg >> 5) & 63, bb = bg & 31;
    int r = (fr * a + br * (256 - a)) >> 8;
    int g = (fgc * a + bgc * (256 - a)) >> 8;
    int b = (fb * a + bb * (256 - a)) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static const uint16_t *media_cover_cache(int *w, int *h) {
    int iw = 0, ih = 0;
    const uint16_t *art = btmap_album_art(&iw, &ih);
    if (!art || iw <= 0 || ih <= 0) return NULL;
    uint32_t seq = btmap_album_art_seq();
    if (!s_cover_cache) {
        s_cover_cache = (uint16_t *)heap_caps_malloc(MEDIA_COVER_W * MEDIA_COVER_H * 2,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cover_cache) return NULL;
    }
    if (seq != s_cover_seq || s_cover_bg != PAL.stage) {
        s_cover_seq = seq;
        s_cover_bg = PAL.stage;
        for (int y = 0; y < MEDIA_COVER_H; y++) {
            int sy = y * ih / MEDIA_COVER_H;
            const uint16_t *row = art + sy * iw;
            for (int x = 0; x < MEDIA_COVER_W; x++) {
                int sx = x * iw / MEDIA_COVER_W;
                /* left fully opaque, right ~90% transparent */
                int a = 256 - (x * 230) / (MEDIA_COVER_W - 1);
                s_cover_cache[y * MEDIA_COVER_W + x] = blend565(row[sx], PAL.stage, a);
            }
        }
    }
    if (w) *w = MEDIA_COVER_W;
    if (h) *h = MEDIA_COVER_H;
    return s_cover_cache;
}

static int media_char_w(int scale) {
    int w = gfx_text_w("M", scale);
    return w > 0 ? w : (scale ? 6 * scale : 6);
}

static int media_utf8_len(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p[0]) return 0;
    if (p[0] < 0x80) return 1;
    if ((p[0] & 0xE0) == 0xC0) return 2;
    if ((p[0] & 0xF0) == 0xE0) return 3;
    if ((p[0] & 0xF8) == 0xF0) return 4;
    return 1;
}

/* Draw a line-oriented word/character wrapped text. Returns line count. */
static int media_wrap(const char *s, int x, int y, int w, int scale,
                      int max_lines, int line_h, uint16_t col) {
    if (!s || !*s) return 0;
    int lines = 0;
    char buf[256];
    while (*s && lines < max_lines) {
        int n = 0, last_space = -1;
        while (s[n]) {
            int l = media_utf8_len(s + n);
            if (n + l >= (int)sizeof(buf) - 1) break;
            memcpy(buf, s, (size_t)(n + l));
            buf[n + l] = 0;
            if (gfx_text_i18n_w(buf, scale) > w - 2) break;
            n += l;
            if (s[n - 1] == ' ') last_space = n;
        }
        if (n == 0) n = media_utf8_len(s);
        int take = n;
        if (s[n] && last_space > 0) take = last_space;
        if (take <= 0) take = n;
        memcpy(buf, s, (size_t)take);
        buf[take] = 0;
        gfx_text_i18n(x, y + lines * line_h, buf, col, scale);
        s += take;
        while (*s == ' ') s++;
        lines++;
    }
    return lines;
}

static void media_scroll_text(const char *s, int x, int y, int w, int scale, uint16_t col) {
    if (!s || !*s) return;
    int tw = gfx_text_i18n_w(s, scale);
    if (tw <= w) { gfx_text_i18n(x, y, s, col, scale); return; }
    gfx_set_clip(x, y, w, scale >= 2 ? 18 : (scale == 1 ? 13 : 9));
    int span = tw + 26;
    int off = (int)(g.now * 0.024f) % span;
    gfx_text_i18n(x - off, y, s, col, scale);
    gfx_text_i18n(x - off + span, y, s, col, scale);
    gfx_set_clip(0, 0, CW, CH);
}

static uint16_t media_bg_color(void) {
    int iw = 0, ih = 0;
    const uint16_t *art = btmap_album_art(&iw, &ih);
    if (!art || iw <= 0 || ih <= 0) return PAL.stage;

    /* Use the same accent value as the progress/volume bars. Compute it
     * directly every frame instead of keeping a second cache that could go
     * stale when the track metadata changes. */
    uint16_t accent = btmap_album_accent565();
    bool dark = btmap_album_black_dominant();
    if (dark || !accent) {
        static uint32_t logged_dark_seq = 0xFFFFFFFFu;
        uint32_t seq = btmap_album_art_seq();
        if (seq != logged_dark_seq) {
            logged_dark_seq = seq;
            ESP_LOGI("media", "bg seq=%lu accent=%04x dark=%d -> stage",
                     (unsigned long)seq, accent, dark);
        }
        return PAL.stage;
    }
    uint16_t out = blend565(accent, PAL.stage, 102);                  /* 40% accent */
    static uint32_t logged_seq = 0xFFFFFFFFu;
    uint32_t seq = btmap_album_art_seq();
    if (seq != logged_seq) {
        logged_seq = seq;
        ESP_LOGI("media", "bg seq=%lu accent=%04x out=%04x dark=0",
                 (unsigned long)seq, accent, out);
    }
    return out;
}

static void media_glass_box(int x, int y, int w, int h, bool pressed) {
    /* Simulated liquid glass: dark translucent tint + bright top edge. */
    gfx_fill(x, y, w, h, pressed ? PAL.seltint : PAL.surface);
    gfx_fill(x + 1, y + 1, w - 2, h / 2, PAL.surface2);
    gfx_hline(x + 2, y + 2, w - 4, PAL.line2);
    gfx_rect(x, y, w, h, pressed ? PAL.cyan : PAL.line);
}

static void media_clock(char *out, int cap, int ms) {
    if (ms < 0) ms = 0;
    int total = (ms + 500) / 1000;
    snprintf(out, cap, "%d:%02d", total / 60, total % 60);
}

static bool s_lyr_menu;
static int s_lyr_menu_sel;
static const char *LYR_MODES[5] = {"AUTO", "LRCLIB", "NETEASE", "NOTIF", "TITLE"};

static void media_lyric_menu(void) {
    const int x = 116, y = 12, w = 194;
    gfx_fill(x, y, w, 204, PAL.panel);
    gfx_rect(x, y, w, 204, PAL.cyan);
    gfx_text(x + 8, y + 6, "LYRIC SOURCE", PAL.cyan, 1);
    for (int i = 0; i < 6; i++) {
        int ry = y + 22 + i * 23;
        bool sel = i == s_lyr_menu_sel;
        bool cur = i == btmap_media_lyric_mode();
        if (sel) gfx_fill(x + 5, ry - 2, w - 10, 21, PAL.seltint);
        if (i < 5) {
            gfx_text(x + 12, ry + 3, LYR_MODES[i], sel ? PAL.white : PAL.text, 1);
            if (cur) gfx_text_right(x + w - 12, ry + 3, "*", PAL.cyan, 1);
        } else {
            char b[24];
            snprintf(b, sizeof b, "NE OFFSET %dms", btmap_media_lyric_offset());
            gfx_text(x + 12, ry + 3, b, sel ? PAL.white : PAL.text, 0);
        }
    }
    gfx_text(x + 8, y + 178, "UP/DN  L/R OFFSET  PUSH  BACK", PAL.dim, 0);
}

static void render_media(void) {
    char b[96];
    if (!g.entered) {
        gfx_fill(0, 0, CW, CH, PAL.stage);
        gfx_text_center(CW / 2, 56, "MEDIA", PAL.cyan, 2);
        gfx_text_center(CW / 2, 96, "PHONE MUSIC CONTROL", PAL.white, 1);
        gfx_text_center(CW / 2, 126, btmap_connected() ? "BT CONNECTED" : "BT NO LINK",
                        btmap_connected() ? PAL.green : PAL.red, 1);
        gfx_text_center(CW / 2, 160, "PUSH TO ENTER", PAL.yellow, 1);
        gfx_text_center(CW / 2, 210, "L/R PAGE - BACK HOME", PAL.dim, 0);
        return;
    }

    gfx_fill(0, 0, CW, CH, media_bg_color());
    bool playing = btmap_media_playing();
    uint16_t accent = btmap_album_accent565();
    uint16_t bar_col = accent ? accent :
        (btmap_album_black_dominant() ? PAL.white : PAL.cyan);

    /* Original-size cover, top-left. */
    int ax = 14, ay = 10, aw = 96, ah = 96;
    int iw = 0, ih = 0;
    const uint16_t *art = btmap_album_art(&iw, &ih);
    bool art_ok = art && iw == aw && ih == ah;
    static bool dbg_art_ok;
    static uint32_t dbg_art_seq = 0xFFFFFFFFu;
    uint32_t art_seq = btmap_album_art_seq();
    if (art_ok != dbg_art_ok || (art_ok && art_seq != dbg_art_seq)) {
        dbg_art_ok = art_ok;
        dbg_art_seq = art_seq;
        ESP_LOGI("media", "cover draw ok=%d seq=%lu art=%p %dx%d",
                 art_ok, (unsigned long)art_seq, (const void *)art, iw, ih);
    }
    if (art_ok) {
        gfx_blit(ax, ay, art, iw, ih, iw);
    } else {
        gfx_fill(ax, ay, aw, ah, PAL.panel);
        gfx_rect(ax, ay, aw, ah, PAL.line);
        gfx_disc(ax + aw / 2, ay + ah / 2, 26, PAL.surface2);
        gfx_circle(ax + aw / 2, ay + ah / 2, 26, PAL.dim);
        gfx_disc(ax + aw / 2, ay + ah / 2, 5, PAL.cyan);
        gfx_text_center(ax + aw / 2, ay + ah - 13, "NO ART", PAL.dim, 0);
    }
    gfx_rect(ax, ay, aw, ah, PAL.line2);

    /* Temporary cover diagnostics: source and actual framebuffer checksums. */
    static int64_t cover_dbg_us;
    int64_t cover_now = esp_timer_get_time();
    if (cover_now - cover_dbg_us > 1000000LL) {
        cover_dbg_us = cover_now;
        uint32_t src_h = 2166136261u, scr_h = 2166136261u;
        if (art_ok) {
            const uint8_t *sp = (const uint8_t *)art;
            for (int i = 0; i < aw * ah * 2; i++) src_h = (src_h ^ sp[i]) * 16777619u;
            const uint16_t *fb = gfx_fb();
            for (int y = 0; y < ah; y++) {
                const uint8_t *rp = (const uint8_t *)(fb + (size_t)(ay + y) * LCD_W + ax);
                for (int i = 0; i < aw * 2; i++) scr_h = (scr_h ^ rp[i]) * 16777619u;
            }
        }
        ESP_LOGI("media", "cover dbg ok=%d seq=%lu src=%08lx scr=%08lx",
                 art_ok, (unsigned long)art_seq, (unsigned long)src_h, (unsigned long)scr_h);
    }

    /* Metadata stays in the current column, enlarged; long values scroll. */
    int rx = 124, rw = 166;
    const char *title = btmap_media_title();
    const char *artist = btmap_media_artist();
    const char *album = btmap_media_album();
    media_scroll_text(title[0] ? title : "NO TRACK", rx, 28, rw, 2, PAL.white);
    media_scroll_text(artist[0] ? artist : "UNKNOWN ARTIST", rx, 52, rw, 1, PAL.text2);
    media_scroll_text(album, rx, 70, rw, 1, PAL.dim);
    const char *lyr_src = btmap_media_lyric_src();
    snprintf(b, sizeof b, "LYR:%s", (lyr_src && lyr_src[0]) ? lyr_src : "--");
    gfx_text(rx, 84, b, PAL.dim, 0);

    /* Volume rail, right, above lyrics. */
    int vol = -1, vmax = 0;
    if (btmap_media_volume(&vol, &vmax)) {
        int pct = (vol * 100 + vmax / 2) / vmax;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int vx = CW - 18, vy = 30, vh = 70;
        gfx_fill(vx, vy, 7, vh, PAL.surface2);
        gfx_fill(vx, vy + vh - (vh * pct) / 100, 7, (vh * pct) / 100, bar_col);
        gfx_rect(vx, vy, 7, vh, PAL.line);
        snprintf(b, sizeof b, "%d", pct);
        gfx_text_right(CW - 14, 104, b, bar_col, 0);
    }

    /* Current and next lyric, full width, wrapped without truncation. The
     * current line is stable at double size; the next line is single size. */
    int ly_active = 0;
    char lyrics[5][192];
    if (btmap_media_lyrics(&ly_active, lyrics)) {
        const char *cur = lyrics[ly_active];
        const char *next = (ly_active + 1 < 5) ? lyrics[ly_active + 1] : "";
        int lines = media_wrap(cur, 10, 112, CW - 20, 2, 2, 18, PAL.cyan);
        int ny = 112 + lines * 18 + 8;
        if (next[0]) media_wrap(next, 10, ny, CW - 20, 1, 2, 13, PAL.dim);
    } else {
        gfx_text(10, 112, "NO LYRICS", PAL.dim, 0);
    }

    /* Low, full-width progress immediately above the transport row. */
    int pos = 0, dur = 0;
    if (btmap_media_progress(&pos, &dur)) {
        char tb[20];
        media_clock(tb, sizeof tb, pos);
        gfx_text(10, 184, tb, PAL.dim, 0);
        media_clock(tb, sizeof tb, dur);
        gfx_text_right(CW - 14, 184, tb, PAL.dim, 0);
        gfx_fill(10, 196, CW - 20, 5, PAL.surface2);
        gfx_fill(10, 196, ((CW - 20) * pos) / dur, 5, bar_col);
        gfx_rect(10, 196, CW - 20, 5, PAL.line);
    }

    /* Flat glass transport row, flush with the bottom edge. */
    const int by = 216, bh = 24, gap = 14;
    const int bw = 78;
    const int bx0 = (CW - (bw * 3 + gap * 2)) / 2;
    int bx[3] = { bx0, bx0 + bw + gap, bx0 + 2 * (bw + gap) };
    for (int i = 0; i < 3; i++) media_glass_box(bx[i], by, bw, bh, media_pressed(i));
    media_flat_prev(bx[0] + bw / 2, by + bh / 2, media_pressed(0) ? PAL.yellow : PAL.white);
    media_flat_next(bx[2] + bw / 2, by + bh / 2, media_pressed(2) ? PAL.yellow : PAL.white);
    if (playing) media_flat_pause(bx[1] + bw / 2, by + bh / 2, media_pressed(1) ? PAL.yellow : PAL.white);
    else media_flat_play(bx[1] + bw / 2, by + bh / 2, media_pressed(1) ? PAL.yellow : PAL.white);

    if (s_lyr_menu) media_lyric_menu();
}

/* ================= BUTTON DEBUG (V4 RKJXM1015004) ================= */
static const char *btn_dir_name(const btn_debug_t *b) {
    int n = (b->up ? 1 : 0) + (b->down ? 1 : 0) + (b->left ? 1 : 0) + (b->right ? 1 : 0);
    if (n > 1) return "MULTI";
    if (b->up) return "UP";
    if (b->down) return "DOWN";
    if (b->left) return "LEFT";
    if (b->right) return "RIGHT";
    if (b->push) return "CENTER";
    return "RELEASED";
}

static void btn_arrow_box(int cx, int cy, int w, int h, int dir, bool active) {
    uint16_t edge = active ? PAL.cyan : PAL.line;
    uint16_t fill = active ? PAL.seltint : PAL.surface;
    gfx_fill(cx - w / 2, cy - h / 2, w, h, fill);
    gfx_rect(cx - w / 2, cy - h / 2, w, h, edge);
    if (active) gfx_rect(cx - w / 2 + 2, cy - h / 2 + 2, w - 4, h - 4, PAL.cyan);
    int ax[3], ay[3];
    int tip = 8, wide = 10, base = 7;
    if (dir == 0) {          /* up */
        ax[0] = cx; ay[0] = cy - tip;
        ax[1] = cx - wide; ay[1] = cy + base;
        ax[2] = cx + wide; ay[2] = cy + base;
    } else if (dir == 1) {   /* down */
        ax[0] = cx; ay[0] = cy + tip;
        ax[1] = cx - wide; ay[1] = cy - base;
        ax[2] = cx + wide; ay[2] = cy - base;
    } else if (dir == 2) {   /* left */
        ax[0] = cx - tip - 2; ay[0] = cy;
        ax[1] = cx + base; ay[1] = cy - wide;
        ax[2] = cx + base; ay[2] = cy + wide;
    } else {                 /* right */
        ax[0] = cx + tip + 2; ay[0] = cy;
        ax[1] = cx - base; ay[1] = cy - wide;
        ax[2] = cx - base; ay[2] = cy + wide;
    }
    gfx_poly_fill(ax, ay, 3, active ? PAL.cyan : PAL.glyphoff);
    if (active) {
        gfx_line(ax[0], ay[0], ax[1], ay[1], PAL.white);
        gfx_line(ax[0], ay[0], ax[2], ay[2], PAL.white);
    }
}

static void render_btn_debug(void) {
    btn_debug_t b;
    input_debug(&b);
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("BUTTON DEBUG", 5);
    gfx_text(6, 22, "RKJXM1015004 / V4.0", PAL.dim, 1);

    bool dir = b.up || b.down || b.left || b.right;
    int cx = CW / 2, cy = 114;
    btn_arrow_box(cx, 56, 46, 30, 0, b.up);
    btn_arrow_box(cx, 172, 46, 30, 1, b.down);
    btn_arrow_box(84, cy, 46, 30, 2, b.left);
    btn_arrow_box(CW - 84, cy, 46, 30, 3, b.right);

    bool center = b.push && !dir;
    uint16_t cfill = center ? PAL.cyan : PAL.surface;
    uint16_t cedge = center ? PAL.white : (b.push ? PAL.yellow : PAL.line);
    gfx_disc(cx, cy, 28, cfill);
    gfx_circle(cx, cy, 28, cedge);
    gfx_circle(cx, cy, 22, cedge);
    gfx_text_center(cx, cy - 5, "PUSH", center ? PAL.white : PAL.dim, 1);

    const char *ds = btn_dir_name(&b);
    uint16_t dc = !strcmp(ds, "RELEASED") ? PAL.dim : PAL.cyan;
    gfx_text_center(cx, 198, ds, dc, 2);
    gfx_text(6, 216, b.push ? "COMMON LOW" : "COMMON HIGH",
             b.push ? PAL.yellow : PAL.dim, 0);
    gfx_text_right(CW - 6, 216, b.back ? "BACK LOW" : "BACK HIGH",
                   b.back ? PAL.cyan : PAL.dim, 0);
    gfx_text_center(cx, 229, "BACK EXIT", PAL.dim, 0);
}

/* ================= CHINA MAP (flat map + GPS) ================= */
#define LMAP_X 4
#define LMAP_Y 16
#define LMAP_W CN_MAPW
#define LMAP_H CN_MAPH

static void render_location(void) {
    gps_state_t *gp = gps_get();
    char b[40], b2[40];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("CHINA MAP", 4);

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

    /* GPS info (bottom of the tall canvas) */
    int ty = CH - 60;
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
static bool read_real_imu(imu6_t *out) {
    memset(out, 0, sizeof(*out));
    if (!g.imu_ok) return false;
    float tp;
    return mpu6050_read(&out->ax, &out->ay, &out->az,
                        &out->gx, &out->gy, &out->gz, &tp);
}

static imu6_t s_imu_win[IMU_STABLE_WIN];
static int    s_imu_win_n = 0;
static int    s_imu_win_i = 0;

static void imu_win_reset(void) {
    s_imu_win_n = 0;
    s_imu_win_i = 0;
}

static void imu_win_push(const imu6_t *s) {
    s_imu_win[s_imu_win_i] = *s;
    s_imu_win_i = (s_imu_win_i + 1) % IMU_STABLE_WIN;
    if (s_imu_win_n < IMU_STABLE_WIN) s_imu_win_n++;
}

/* Require both low variance and no isolated spike.  The old implementation
 * used the whole 100-sample rolling buffer, so motion could keep the screen
 * "MOVING" for several seconds after the device was already still. */
static bool imu_win_is_stable(void) {
    if (s_imu_win_n < IMU_STABLE_WIN) return false;
    float ma[3] = {0, 0, 0}, mg[3] = {0, 0, 0};
    for (int i = 0; i < s_imu_win_n; i++) {
        const imu6_t *s = &s_imu_win[i];
        ma[0] += s->ax; ma[1] += s->ay; ma[2] += s->az;
        mg[0] += s->gx; mg[1] += s->gy; mg[2] += s->gz;
    }
    for (int k = 0; k < 3; k++) {
        ma[k] /= (float)s_imu_win_n;
        mg[k] /= (float)s_imu_win_n;
    }
    float va[3] = {0, 0, 0}, vg[3] = {0, 0, 0};
    float pa = 0, pg = 0;
    for (int i = 0; i < s_imu_win_n; i++) {
        const imu6_t *s = &s_imu_win[i];
        float a[3] = { s->ax, s->ay, s->az };
        float g[3] = { s->gx, s->gy, s->gz };
        for (int k = 0; k < 3; k++) {
            float da = a[k] - ma[k];
            float dg = g[k] - mg[k];
            va[k] += da * da;
            vg[k] += dg * dg;
            if (fabsf(da) > pa) pa = fabsf(da);
            if (fabsf(dg) > pg) pg = fabsf(dg);
        }
    }
    /* Do not gate calibration on |a| == 1 g. This board's accelerometer
     * reports a consistently lower magnitude (~0.4 g), and rejecting it made
     * a still board look like free-fall forever. Variance/peak checks plus the
     * gravity direction are sufficient; app_apply_imu_reference normalizes it. */
    if (pa > IMU_STABLE_PEAK_A || pg > IMU_STABLE_PEAK_G) return false;
    for (int k = 0; k < 3; k++) {
        if (sqrtf(va[k] / (float)s_imu_win_n) > IMU_STABLE_TILT) return false;
        if (sqrtf(vg[k] / (float)s_imu_win_n) > IMU_STABLE_GYRO) return false;
    }
    return true;
}

static float imu_frac(void) {
    float f = g.imu_progress / IMU_CALIB_MS;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return f;
}

static int imu_pct(void) {
    int p = (int)(imu_frac() * 100.0f + 0.5f);
    if (p > 100) p = 100;
    if (p < 0) p = 0;
    return p;
}

static void imu_bubble(void) {
    gfx_fill(0, 0, 176, CH, PAL.stage);
    float cx = 88, cy = 112, R = 74;
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
        float bx = cx + (s.ax - g.i_ox) * scale, by = cy + (s.ay - g.i_oy) * scale;
        float off = hypotf(s.ax - g.i_ox, s.ay - g.i_oy);
        uint16_t col = off < 0.03f ? PAL.green : (off < 0.08f ? PAL.yellow : PAL.red);
        gfx_fill((int)bx - 3, (int)by - 3, 6, 6, col);
        gfx_fill((int)bx - 1, (int)by - 1, 2, 2, PAL.mapbg);
    }
    int seg = 64;
    float frac = imu_frac();
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
    gfx_fill(rx + 1, 93, (int)(118 * imu_frac()), 6, PAL.cyan);
    snprintf(b, sizeof b, "%d%%", imu_pct());
    gfx_text_center(rx + 60, 104, b, PAL.cyan, 1);
    gfx_text(rx, 150, "PUSH FORCE  2xPUSH REDO", PAL.dim, 0);
    gfx_text(rx, 166, "BACK SKIP", PAL.dim, 0);
    gfx_text(rx, 210, "KEEP DEVICE STILL - BACK SKIP", PAL.dim, 0);
}

/* ================= STATIC COVER TEST ================= */
#define COVER_TEST_W 96
static uint16_t *s_cover_test_buf;
static bool s_cover_test_placeholder;

static void cover_test_capture(void) {
    if (!s_cover_test_buf) {
        s_cover_test_buf = (uint16_t *)heap_caps_malloc(
            COVER_TEST_W * COVER_TEST_W * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_cover_test_buf) return;
    int iw = 0, ih = 0;
    const uint16_t *art = btmap_album_art(&iw, &ih);
    if (art && iw == COVER_TEST_W && ih == COVER_TEST_W) {
        memcpy(s_cover_test_buf, art, COVER_TEST_W * COVER_TEST_W * 2);
        s_cover_test_placeholder = false;
        ESP_LOGI("media", "cover test captured seq=%lu", (unsigned long)btmap_album_art_seq());
    } else {
        static const uint16_t cols[8] = {
            RGB565(255,255,255), RGB565(255,255,0), RGB565(0,255,255),
            RGB565(0,255,0), RGB565(255,0,255), RGB565(255,0,0),
            RGB565(0,0,255), RGB565(0,0,0)
        };
        for (int y = 0; y < COVER_TEST_W; y++)
            for (int x = 0; x < COVER_TEST_W; x++)
                s_cover_test_buf[y * COVER_TEST_W + x] = cols[(x * 8) / COVER_TEST_W];
        s_cover_test_placeholder = true;
        ESP_LOGI("media", "cover test captured placeholder");
    }
}

static void render_cover_test(void) {
    gfx_fill(0, 0, CW, CH, PAL.stage);
    if (!s_cover_test_buf) {
        gfx_text_center(CW / 2, 80, "NO BUFFER", PAL.red, 1);
        return;
    }
    const int size = 192, x0 = (CW - size) / 2, y0 = 4;
    uint16_t *fb = gfx_fb();
    for (int y = 0; y < size; y++) {
        const uint16_t *src = s_cover_test_buf + (y * COVER_TEST_W / size) * COVER_TEST_W;
        uint16_t *dst = fb + (size_t)(y0 + y) * LCD_W + x0;
        for (int x = 0; x < size; x++) dst[x] = src[x * COVER_TEST_W / size];
    }
    gfx_rect(x0 - 1, y0 - 1, size + 2, size + 2, PAL.white);
    gfx_text(4, 4, "COVER", PAL.cyan, 0);
    gfx_text(4, 16, "HOLD", PAL.cyan, 0);
    gfx_text(4, 90, s_cover_test_placeholder ? "NO ART" : "STATIC", PAL.dim, 0);
    gfx_text(4, 190, "BACK", PAL.dim, 0);
}

/* ================= MEDIA COLOUR TUNING ================= */
static void render_color_tune(void) {
    char b[48];
    gfx_fill(0, 0, CW, CH, PAL.stage);
    title_c("MEDIA COLOR", 2);

    int iw = 0, ih = 0;
    const uint16_t *art = btmap_album_art(&iw, &ih);
    if (art && iw == 96 && ih == 96) {
        gfx_blit(10, 32, art, iw, ih, iw);
    } else {
        gfx_fill(10, 32, 96, 96, PAL.panel);
        gfx_text_center(58, 74, "NO ART", PAL.dim, 0);
    }
    gfx_rect(10, 32, 96, 96, PAL.line2);

    static const char *keys[2] = {"BLACK%", "NEAR BLK"};
    int vals[2] = {g.media_black_pct, g.media_near_black};
    const int rx = 116, rw = 194, rh = 28;
    for (int i = 0; i < 2; i++) {
        int y = 32 + i * 36;
        bool on = i == g.sel;
        panel_box(rx, y, rw, rh, on, false);
        gfx_text(rx + 8, y + 9, keys[i], PAL.text, 1);
        if (i == 1) snprintf(b, sizeof b, "%d", vals[i]);
        else snprintf(b, sizeof b, "%d%%", vals[i]);
        gfx_text_right(rx + rw - 8, y + 9, b, on ? PAL.yellow : PAL.cyan, 1);
    }

    uint16_t accent = btmap_album_accent565();
    uint16_t bar = accent ? accent :
        (btmap_album_black_dominant() ? PAL.white : PAL.cyan);
    gfx_fill(10, 140, 96, 34, media_bg_color());
    gfx_rect(10, 140, 96, 34, PAL.line2);
    gfx_text(14, 147, "BG 40%", PAL.white, 0);
    gfx_fill(rx, 140, rw, 12, bar);
    gfx_rect(rx, 140, rw, 12, PAL.line2);
    snprintf(b, sizeof b, "ACCENT %04X", accent);
    gfx_text(rx, 158, b, PAL.dim, 0);
    gfx_text(rx, 172, btmap_album_black_dominant() ? "BLACK DOMINANT" : "COLOUR DOMINANT",
             btmap_album_black_dominant() ? PAL.white : PAL.dim, 0);
    hint_c("UP/DN SELECT - L/R ADJUST - PUSH SAVE - BACK", HINT_Y);
}

/* ================= dispatch ================= */
void screens_render(void) {
    switch (g.cur) {
    case SC_HOME: render_home(); break;
    case SC_RACE_MENU: render_race_menu(); break;
    case SC_NAV_MENU: render_nav_menu(); break;
    case SC_PERF: render_perf(); break;
    case SC_NAV: render_nav(); break;
    case SC_PHONE: render_phone(); break;
    case SC_MEDIA: render_media(); break;
    case SC_WIFI_SETUP: render_wifi_setup(); break;
    case SC_TRACK_EDIT: render_track_edit(); break;
    case SC_SETTINGS: render_settings(); break;
    case SC_MODE_SELECT: render_mode_select(); break;
    case SC_MOUNT_SELECT: render_mount_select(); break;
    case SC_NEW_TRACK_MARK_START: render_record_track(); break;
    case SC_REC_LAP_SELECT: render_rec_lap_select(); break;
    case SC_REC_PREVIEW: render_rec_preview(); break;
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
    case SC_BTN_DEBUG: render_btn_debug(); break;
    case SC_LOCATION: render_location(); break;
    case SC_SOUND: render_sound(); break;
    case SC_FLASH: render_flash(); break;
    case SC_COVER_TEST: render_cover_test(); break;
    case SC_COLOR_TUNE: render_color_tune(); break;
    case SC_IMU_DEBUG: render_imu_debug(); break;
    case SC_TRACK_START: render_track_start(); break;
    case SC_IMU_CALIB: render_imu_calib(); break;
    default: break;
    }
}

/* ================= enter ================= */
void screens_enter(scid_t id) {
    s_map_ready = false;
    nav_set_active(false);
    switch (id) {
    case SC_IMU_CALIB:
        g.imu_phase = 0; g.imu_ns = 0; g.imu_progress = 0; g.imu_stable = false;
        g.imu_doneTimer = 0;
        imu_win_reset();
        mpu_clear_calibration(&g.imu);
        break;
    case SC_RACE_MENU: g.sel = 0; break;
    case SC_NAV_MENU: g.sel = 0; break;
    case SC_TRACK_EDIT: g.sel = 0; g.entered = false; break;
    case SC_SETTINGS: g.sel = g.settings_sel; g.entered = true; g.set_edit = false; break;
    case SC_MODE_SELECT: g.sel = 0; break;
    case SC_MOUNT_SELECT: g.sel = g.race_mount; break;
    case SC_PERF:
        g.entered = false; g.perf_state = 0; g.perf_time = 0; g.perf_trig_cnt = 0;
        g.perf_mode_sel = g.perf_mode;
        g.perf_custom_focus = 0;
        g.perf_edit = false; g.perf_edit_anim = 0;
        g.sel = 0;
        break;
    case SC_NAV: g.entered = false; g.nav_menu = 0; g.nav_sel = 0; break;
    case SC_PHONE: g.entered = false; break;
    case SC_MEDIA: g.entered = false; s_media_hit = -1; s_lyr_menu = false; break;
    case SC_WIFI_SETUP: netcfg_start_ap(); break;
    case SC_REC_LAP_SELECT:
        g.rec_sel_cursor = 0;
        g.rec_lap_sel = 0;
        for (int i = 0; i < g.rec_lap_n; i++) g.rec_lap_sel |= (1u << i);
        break;
    case SC_REC_PREVIEW:
        break;
    case SC_NEW_TRACK_MARK_START:
        g.rec_state = 0; app_rec_reset(); g.rec_has_ref = false;
        g.rec_line_ok = false; g.rec_armed = false; g.rec_along_prev = 0;
        g.rec_laps = 0; g.rec_last_lap = g.rec_best_lap = g.rec_delta = 0;
        g.rec_lap_time = 0;
        g.rec_last_fix_seq = 0; g.rec_prev_fix_us = 0; g.rec_lap_start_us = 0;
        g.session_nsec = 0;
        break;
    case SC_RACING: case SC_RACING_RECORDED:
        g.panel = 0;
        if (g.engine_valid) {
            engine_set_profile(&g.engine, g.race_mount);
            g.engine.logged = false;
            if (g.engine.track) {
                race_store_begin(g.engine.track->store_slot,
                                 g.engine.track->name, id == SC_RACING ? 0 : 1);
            }
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
    case SC_TRACK_START: {
        track_t *t = g.engine.track;
        g.start_f = 0;
        if (t && t->geo && t->nscale > 0) {
            geo_ref_t r = { t->glat0, t->glon0 };
            float mx, my;
            geo_meters(&r, t->line_lat, t->line_lon, &mx, &my);
            pt_t q = { mx * t->nscale + t->nox, my * t->nscale + t->noy };
            float s = 0, dd = 0;
            if (track_project(t, q, &s, &dd)) g.start_f = s / (float)t->N;
        }
        break;
    }
    case SC_SOUND: g.sel = 0; break;
    case SC_IMU_DEBUG: g.i_gmax = 0; break;   /* keep the boot-pose zero offsets */
    case SC_GPS_DEBUG: gps_begin(); g.dbg_page = 0; break;
    case SC_BTN_DEBUG: break;
    case SC_FLASH: break;
    case SC_COVER_TEST: cover_test_capture(); break;
    case SC_COLOR_TUNE: g.sel = 0; break;
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
    /* auto power saving: apply after page init so it is not overridden */
    gps_set_nav_rate(screen_gps_hz(id));
}

/* ================= update ================= */
void screens_update(float dt) {
    engine_t *e = &g.engine;
    switch (g.cur) {
    case SC_IMU_CALIB: {
        if (g.imu_phase == 0) {
            imu6_t s;
            if (!read_real_imu(&s)) {
                /* A failed I2C read used to become a zero sample, which looked
                 * like violent motion and reset the entire stable window. */
                static int64_t last_warn;
                int64_t now = esp_timer_get_time();
                if (now - last_warn > 5000000) {
                    last_warn = now;
                    ESP_LOGW("imu", "calibration read failed; holding state");
                }
                break;
            }
            imu_win_push(&s);
            bool stable = imu_win_is_stable();
            if (!stable) {
                /* Any real motion invalidates the run; the next still period
                 * starts a fresh calibration instead of reusing old samples. */
                g.imu_stable = false;
                g.imu_ns = 0;
                g.imu_progress = 0;
            } else {
                if (!g.imu_stable) {
                    g.imu_ns = 0;
                    g.imu_progress = 0;
                }
                g.imu_stable = true;
                if (g.imu_ns < 100) g.imu_samples[g.imu_ns++] = s;
                g.imu_progress += dt * 1000.0f;
            }
            if (g.imu_phase == 0 && g.imu_progress >= IMU_CALIB_MS) {
                imu6_t m = {0, 0, 0, 0, 0, 0};
                int n2 = g.imu_ns;
                if (n2) {
                    for (int i = 0; i < n2; i++) {
                        m.ax += g.imu_samples[i].ax; m.ay += g.imu_samples[i].ay; m.az += g.imu_samples[i].az;
                        m.gx += g.imu_samples[i].gx; m.gy += g.imu_samples[i].gy; m.gz += g.imu_samples[i].gz;
                    }
                    m.ax /= n2; m.ay /= n2; m.az /= n2; m.gx /= n2; m.gy /= n2; m.gz /= n2;
                }
                app_apply_imu_reference(&m);
                ESP_LOGI("imu", "calibrated: a(%.3f,%.3f,%.3f) g(%.1f,%.1f,%.1f)",
                         m.ax, m.ay, m.az, m.gx, m.gy, m.gz);
                g.i_gmax = 0;
                g.imu_progress = IMU_CALIB_MS;
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
    case SC_PERF:
        perf_update(dt);
        break;
    case SC_NEW_TRACK_MARK_START: {
        gps_state_t *gp = gps_get();
        if (g.rec_state != 1 || !gp->has_pos || gp->fix_type < 2) break;
        if (!g.rec_has_ref) {
            g.rec_has_ref = true;
            g.rec_lat0 = gp->lat; g.rec_lon0 = gp->lon;
            g.rec_n = 0; g.rec_dist = 0;
            if (!app_rec_reserve(1)) break;
            g.rec_pts[g.rec_n++] = (pt_t){0, 0};
            g.rec_last = (pt_t){0, 0};
            g.rec_lap_start_us = gp->fix_time_valid ? gp->fix_time_us : 0;
            g.rec_prev_fix_us = g.rec_lap_start_us;
            g.rec_last_fix_seq = gp->fix_seq;
            g.rec_line_ok = false; g.rec_armed = false; g.rec_along_prev = 0;
            g.rec_laps = 0; g.rec_last_lap = g.rec_best_lap = g.rec_delta = 0;
            break;
        }
        /* New-track lap timing also follows GNSS fixes, not UI frames. */
        if (gp->fix_seq == g.rec_last_fix_seq) break;
        g.rec_last_fix_seq = gp->fix_seq;
        if (gp->fix_time_valid && !g.rec_lap_start_us) {
            g.rec_lap_start_us = gp->fix_time_us;
            g.rec_prev_fix_us = gp->fix_time_us;
        }
        geo_ref_t r = { g.rec_lat0, g.rec_lon0 };
        float mx, my;
        geo_meters(&r, gp->lat, gp->lon, &mx, &my);
        float d = hypotf(mx - g.rec_last.x, my - g.rec_last.y);
        if (d >= 2.0f) {
            g.rec_dist += d;
            if (app_rec_reserve(g.rec_n + 1)) g.rec_pts[g.rec_n++] = (pt_t){mx, my};
            g.rec_last = (pt_t){mx, my};
        }
        if (gp->fix_time_valid && g.rec_lap_start_us) {
            int64_t elapsed = rec_gnss_delta(g.rec_lap_start_us, gp->fix_time_us);
            g.rec_lap_time = elapsed > 0 ? (float)elapsed / 1e6f : 0.0f;
        }
        /* learn the start-line heading from the first few points */
        if (!g.rec_line_ok && g.rec_n >= 6) {
            float dx = g.rec_pts[5].x - g.rec_pts[0].x;
            float dy = g.rec_pts[5].y - g.rec_pts[0].y;
            if (hypotf(dx, dy) > 1.5f) { g.rec_hdg = atan2f(dy, dx); g.rec_line_ok = true; }
        }
        if (!g.rec_line_ok) break;
        float h = g.rec_hdg;
        float along = mx * cosf(h) + my * sinf(h);
        float latd  = -mx * sinf(h) + my * cosf(h);
        if (!g.rec_armed && hypotf(mx, my) > 20.0f) g.rec_armed = true;
        bool time_ok = gp->fix_time_valid && g.rec_lap_start_us && g.rec_prev_fix_us;
        bool crossed = time_ok && g.rec_armed && g.rec_along_prev < 0.0f && along >= 0.0f &&
                       fabsf(latd) < START_LINE_HALF_M && g.rec_lap_time > 2.0f;
        if (crossed) {
            float frac = (along != g.rec_along_prev) ?
                         -g.rec_along_prev / (along - g.rec_along_prev) : 0.0f;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            int64_t dt_us = rec_gnss_delta(g.rec_prev_fix_us, gp->fix_time_us);
            uint64_t cross_us = g.rec_prev_fix_us + (uint64_t)(frac * (dt_us > 0 ? dt_us : 0));
            int64_t lap_us = rec_gnss_delta(g.rec_lap_start_us, cross_us);
            float lap_time = lap_us > 0 ? (float)lap_us / 1e6f : g.rec_lap_time;
            /* crossed the start line: keep this lap, start the next one */
            track_build_geo(&g.sessionTrack, g.rec_pts, g.rec_n,
                            g.rec_lat0, g.rec_lon0, "NEW", 0);
            g.sessionTrack_valid = true;
            g.rec_lap_time = lap_time;
            rec_capture_lap();
            g.rec_laps++;
            g.rec_last_lap = lap_time;
            if (g.rec_laps == 1 || lap_time < g.rec_best_lap) g.rec_best_lap = lap_time;
            g.rec_delta = g.rec_last_lap - g.rec_best_lap;
            g.rec_flash = 0.48f;
            g.rec_n = 0; g.rec_dist = 0;
            if (app_rec_reserve(1)) g.rec_pts[g.rec_n++] = (pt_t){mx, my};
            g.rec_last = (pt_t){mx, my};
            g.rec_lap_start_us = cross_us;
            g.rec_lap_time = (float)(rec_gnss_delta(cross_us, gp->fix_time_us) / 1e6f);
            if (g.rec_lap_time < 0) g.rec_lap_time = 0;
            g.rec_armed = false;
            g.rec_along_prev = along;
        } else {
            g.rec_along_prev = along;
        }
        if (gp->fix_time_valid) g.rec_prev_fix_us = gp->fix_time_us;
        break;
    }
    case SC_RACING:
    case SC_RACING_RECORDED:
        if (g.engine_valid) {
            gps_state_t *gp = gps_get();
            bool gnss_fix = gp->has_pos && gp->fix_type >= 1 && gp->fix_time_valid;
            if (e->real && gnss_fix) {
                race_store_sample(gp->lat, gp->lon, gp->speed_kmh,
                                  e->lap, true, gp->fix_seq, gp->fix_time_us);
            }
            e->speedMul = g.simSpeed;
            if (e->real) {
                engine_update_gnss(e, dt, gp->lat, gp->lon, gp->speed_kmh,
                                   gnss_fix, gp->fix_seq, gp->fix_time_us);
                /* real G for the G-meter panel */
                float ax, ay, az, gx, gy, gz, tp;
                if (g.imu_ok && mpu6050_read(&ax, &ay, &az, &gx, &gy, &gz, &tp)) {
                    float gl, gn, gu;
                    if (app_imu_axes(ax, ay, az, &gl, &gn, &gu)) {
                        const float k = 0.25f;
                        e->gLat = e->gLat * (1 - k) + gl * k;
                        e->gLong = e->gLong * (1 - k) + gn * k;
                    }
                    e->gTick = (e->gTick + 1) % 2;
                    if (e->gTick == 0) {
                        if (e->nGTrail < 70) e->gTrail[e->nGTrail++] = (gpt_t){e->gLat, e->gLong};
                        else {
                            memmove(&e->gTrail[0], &e->gTrail[1], sizeof(gpt_t) * 69);
                            e->gTrail[69] = (gpt_t){e->gLat, e->gLong};
                        }
                    }
                }
            } else {
                engine_update(e, dt);
            }
            if (e->lap_event) {
                e->lap_event = false;
                g.rec_flash = 0.48f;
            }
        }
        break;
    case SC_LAP_SELECT:
        break;
    case SC_GPS_DEBUG:
        gps_poll();
        break;
    case SC_IMU_DEBUG: {
        float ax, ay, az, gx, gy, gz, tp;
        if (mpu6050_read(&ax, &ay, &az, &gx, &gy, &gz, &tp)) {
            g.imu_ok = true;
            g.i_temp = tp;
            g.i_ax = ax - g.i_ox; g.i_ay = ay - g.i_oy; g.i_az = az - g.i_oz;
            g.i_gx = gx - g.i_ogx; g.i_gy = gy - g.i_ogy; g.i_gz = gz - g.i_ogz;
            float hr, hf, hu;
            if (app_imu_axes(ax, ay, az, &hr, &hf, &hu)) {
                float mag = sqrtf(hr * hr + hf * hf);
                if (mag > g.i_gmax) g.i_gmax = mag;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ---------- callbacks used by confirm modals ---------- */
static void rec_make_log(void) {
    int nsel = rec_selected_count();
    if (!nsel || !g.log) return;
    if (g.nlog >= HISTORY_RECORDS) {
        memmove(&g.log[0], &g.log[1], sizeof(histrec_t) * (HISTORY_RECORDS - 1));
        g.nlog = HISTORY_RECORDS - 1;
    }
    histrec_t *r = &g.log[g.nlog++];
    memset(r, 0, sizeof(*r));
    strncpy(r->trackName, g.sessionTrack.name, sizeof(r->trackName) - 1);
    strcpy(r->date, "NEW");
    r->mode = 0;
    r->vehicle = g.sessionTrack.vehicle;
    r->nsec = g.sessionTrack.nsec;
    for (int i = 0; i < r->nsec; i++) r->sections[i] = g.sessionTrack.sections[i];
    float total = 0;
    int best_lap = -1;
    float best_time = 0;
    for (int i = 0; i < g.rec_lap_n && r->lapCount < HISTORY_LAPS; i++) {
        if (!(g.rec_lap_sel & (1u << i))) continue;
        int k = r->lapCount++;
        r->lapTime[k] = g.rec_lap_data[i].time;
        r->lapIndex[k] = i + 1;
        total += g.rec_lap_data[i].time;
        if (best_lap < 0 || g.rec_lap_data[i].time < best_time) {
            best_lap = i; best_time = g.rec_lap_data[i].time;
        }
    }
    r->duration = total;
    if (best_lap >= 0) {
        r->hasBest = true;
        r->bestTime = best_time;
        r->bestIndex = best_lap + 1;
        for (int j = 0; j < TRACE_MAX; j++) {
            int src = j * TRK_N / TRACE_MAX;
            r->trace[j] = rec_norm_point(g.rec_lap_data[best_lap].points[src]);
        }
        r->nTrace = TRACE_MAX;
    }
    if (!log_store_save(r)) app_toast("LOG FLASH FULL");
}

static void cb_build_fused_track(void) {
    if (!rec_build_fusion()) { app_toast("SELECT AT LEAST 1 LAP"); return; }
    app_go(SC_REC_PREVIEW, 1);
}

static void cb_confirm_fused_track(void) {
    app_go(SC_TRACK_NAMING, 1);
}

static void cb_keep_log_yes(void) {
    rec_make_log();
    app_go_home_to(0);
}

static void cb_keep_log_no(void) {
    app_go_home_to(0);
}

static void cb_track_reverse_yes(void) {
    track_t *t = g.engine.track;
    if (!t) return;
    t->reverse = t->reverse ? 0 : 1;
    app_toast(t->reverse ? "TRACK CCW" : "TRACK CW");
}

static void cb_racing_yes(void) { app_go(SC_LAP_SELECT, 1); }
static void cb_racing_no(void) { app_back(); }
static void cb_save_race_log_yes(void) { app_record_race(1); app_back(); }
static void cb_save_race_log_no(void) { g.engine.logged = true; app_back(); }
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

static void open_race_item(int i) {
    if (i == 0) app_go(SC_MODE_SELECT, 1);
    else if (i == 1) { app_go(SC_HISTORY, 1); g.entered = true; }
    else if (i == 2) { app_go(SC_TRACK_EDIT, 1); g.entered = true; }
    else { app_go(SC_PERF, 1); g.entered = true; g.perf_state = 0; g.perf_time = 0; }
}

static void open_nav_item(int i) {
    if (i == 0) {
        app_go(SC_PHONE, 1);
        g.entered = true;
    } else {
        g.nav_mode = 1;
        g.nav_sat = 100;
        nav_set_mode(1, 100);
        app_save_settings();
        app_go(SC_NAV, 1);
        g.entered = true;
        nav_set_active(true);
        nav_refresh();
    }
}

/* ================= handle ================= */
void screens_handle(const event_t *ev) {
    int d = nav_dir(ev);
    int lr = lr_dir(ev);
    engine_t *e = &g.engine;
    switch (g.cur) {
    case SC_HOME:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_LEFT || ev->button == BTN_RIGHT) {
            g.homeIndex ^= 1;
        } else if (ev->button == BTN_UP || ev->button == BTN_DOWN) {
            g.homeIndex ^= 2;
        } else if (ev->button == BTN_PUSH) {
            if (g.homeIndex == 0) app_go(SC_RACE_MENU, 1);
            else if (g.homeIndex == 1) app_go(SC_NAV_MENU, 1);
            else if (g.homeIndex == 2) { app_go(SC_MEDIA, 1); g.entered = true; }
            else { app_go(SC_SETTINGS, 1); g.entered = true; }
        }
        break;

    case SC_RACE_MENU:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_UP || ev->button == BTN_DOWN) {
            g.sel = (g.sel + (ev->button == BTN_DOWN ? 1 : -1) + 4) % 4;
        } else if (ev->button == BTN_PUSH) open_race_item(g.sel);
        else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_NAV_MENU:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_UP || ev->button == BTN_DOWN) {
            g.sel = (g.sel + (ev->button == BTN_DOWN ? 1 : -1) + 2) % 2;
        } else if (ev->button == BTN_PUSH) open_nav_item(g.sel);
        else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_NAV:
        if (g.nav_menu) {
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_BACK) {
                g.nav_menu = (g.nav_menu == 2) ? 1 : 0;
                g.nav_sel = (g.nav_menu == 1) ? 1 : 0;
                return;
            }
            int cnt = 5;
            if (d) { g.nav_sel = (g.nav_sel + d + cnt) % cnt; return; }
            if (ev->button == BTN_PUSH) {
                if (g.nav_menu == 1) {
                    int i = g.nav_sel;
                    if (i == 0) { nav_refresh(); app_toast("MAP REFRESH"); }
                    else if (i == 1) { g.nav_menu = 2; g.nav_sel = nav_style_idx(); return; }
                    else if (i == 4) {
                        g.nav_full = g.nav_full ? 0 : 1;
                        nav_set_fullscreen(g.nav_full != 0);
                        app_save_settings();
                        app_toast(g.nav_full ? "FULLSCREEN" : "NORMAL VIEW");
                        g.nav_menu = 0;
                    } else {
                        nav_apply_zoom(i == 2 ? 1 : -1);
                        return;                 /* keep menu open for repeated zoom */
                    }
                    if (i == 0) g.nav_menu = 0;
                } else {
                    int i = g.nav_sel;
                    nav_apply_style(i);
                    app_toast(i == 0 ? "ROAD PIXEL" : i == 1 ? "SATELLITE" :
                              i == 2 ? "BLEND 30%" : i == 3 ? "BLEND 60%" : "BLEND 90%");
                    g.nav_menu = 0;
                }
            }
            return;
        }
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) { g.entered = true; nav_set_active(true); nav_refresh(); }
            else if (ev->button == BTN_BACK) app_back();
            break;
        }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { g.nav_menu = 1; g.nav_sel = 0; }
        else if (ev->button == BTN_BACK) { g.entered = false; g.nav_menu = 0; nav_set_active(false); app_back(); }
        break;

    case SC_PHONE:
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_back();
        } else {
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_BACK) app_back();
        }
        break;

    case SC_MEDIA:
        if (g.entered && s_lyr_menu) {
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_UP) { s_lyr_menu_sel = (s_lyr_menu_sel + 5) % 6; return; }
            if (ev->button == BTN_DOWN) { s_lyr_menu_sel = (s_lyr_menu_sel + 1) % 6; return; }
            if (s_lyr_menu_sel == 5 && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
                int ms = btmap_media_lyric_offset() + (ev->button == BTN_RIGHT ? 100 : -100);
                btmap_set_media_lyric_offset(ms);
                char c[28];
                snprintf(c, sizeof c, "LYROFFSET,%d", btmap_media_lyric_offset());
                if (btmap_subscribed()) btmap_media_cmd(c);
                else app_toast("NO BT LINK");
                return;
            }
            if (ev->button == BTN_PUSH || ev->button == BTN_OK) {
                if (s_lyr_menu_sel < 5) {
                    char c[24];
                    snprintf(c, sizeof c, "LYRSET,%d", s_lyr_menu_sel);
                    if (btmap_subscribed()) btmap_media_cmd(c);
                    else app_toast("NO BT LINK");
                }
                s_lyr_menu = false;
                return;
            }
            if (ev->button == BTN_BACK) s_lyr_menu = false;
            return;
        }
        if (g.entered && ev->type == EV_LONG &&
            (ev->button == BTN_PUSH || ev->button == BTN_OK)) {
            s_lyr_menu = true;
            s_lyr_menu_sel = btmap_media_lyric_mode();
            return;
        }
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_back();
        } else {
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_BACK) { app_back(); return; }
            int hit = -1;
            const char *cmd = NULL, *msg = NULL;
            if (ev->button == BTN_UP) { cmd = "VOLUP"; msg = "VOL +"; }
            else if (ev->button == BTN_DOWN) { cmd = "VOLDOWN"; msg = "VOL -"; }
            else if (ev->button == BTN_LEFT) { hit = 0; cmd = "PREV"; msg = "PREV"; }
            else if (ev->button == BTN_RIGHT) { hit = 2; cmd = "NEXT"; msg = "NEXT"; }
            else if (ev->button == BTN_PUSH || ev->button == BTN_OK) {
                hit = 1; cmd = "TOGGLE"; msg = "PLAY/PAUSE";
            }
            if (!cmd) return;
            if (!btmap_subscribed()) { app_toast("NO BT LINK"); return; }
            s_media_hit = hit;
            s_media_hit_us = esp_timer_get_time();
            btmap_media_cmd(cmd);
            app_toast(msg);
        }
        break;

    case SC_WIFI_SETUP:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { netcfg_start_ap(); app_toast("AP RUNNING"); }
        else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_PERF:
        if (!g.entered) {
            if (ev->type == EV_PRESS && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
                int dir = ev->button == BTN_RIGHT ? 1 : -1;
                g.perf_mode_sel = (g.perf_mode_sel + dir + 3) % 3;
                return;
            }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) {
                g.perf_mode = g.perf_mode_sel;
                g.entered = true;
                g.perf_state = 0; g.perf_time = 0; g.perf_custom_focus = 0;
                g.perf_edit = false;
                app_save_settings();
                app_toast(g.perf_mode == 0 ? "0-100 ENTER" : g.perf_mode == 1 ? "100-0 ENTER" : "CUSTOM SETUP");
            } else if (ev->button == BTN_BACK) app_back();
            break;
        }
        if (g.perf_edit) {
            int dir = ev->button == BTN_UP ? 10 : (ev->button == BTN_DOWN ? -10 : 0);
            if ((ev->type == EV_PRESS || ev->type == EV_LONG || ev->type == EV_REPEAT) && dir) {
                int old = g.perf_custom_focus == 0 ? g.perf_speed_a : g.perf_speed_b;
                if (g.perf_custom_focus == 0) g.perf_speed_a += dir;
                else g.perf_speed_b += dir;
                if (g.perf_speed_a < 0) g.perf_speed_a = 0;
                if (g.perf_speed_a > 300) g.perf_speed_a = 300;
                if (g.perf_speed_b < 0) g.perf_speed_b = 0;
                if (g.perf_speed_b > 300) g.perf_speed_b = 300;
                int nv = g.perf_custom_focus == 0 ? g.perf_speed_a : g.perf_speed_b;
                if (nv != old) {
                    g.perf_edit_old = old;
                    g.perf_edit_dir = dir > 0 ? 1 : -1;
                    g.perf_edit_anim = 1.0f;
                    app_save_settings();
                }
                return;
            }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) {
                g.perf_edit = false;
                app_save_settings();
                app_toast("VALUE SET");
            } else if (ev->button == BTN_BACK) {
                g.perf_edit = false;
                if (g.perf_custom_focus == 0) g.perf_speed_a = g.perf_edit_old;
                else g.perf_speed_b = g.perf_edit_old;
                app_save_settings();
                app_toast("EDIT CANCELLED");
            }
            break;
        }
        if (ev->type != EV_PRESS) return;
        if (g.perf_mode == 2 && g.perf_state == 0) {
            if (ev->button == BTN_LEFT) { g.perf_custom_focus = 0; return; }
            if (ev->button == BTN_RIGHT) { g.perf_custom_focus = 1; return; }
            if (ev->button == BTN_UP) {
                if (g.perf_custom_focus == 0) {
                    g.entered = false;
                    g.perf_state = 0;
                    g.perf_mode_sel = g.perf_mode;
                    app_toast("SELECT MODE");
                } else {
                    g.perf_custom_focus--;
                }
                return;
            }
            if (ev->button == BTN_DOWN) {
                g.perf_custom_focus = (g.perf_custom_focus + 1) % 3;
                return;
            }
            if (ev->button == BTN_PUSH) {
                if (g.perf_custom_focus == 2) {
                    if (g.perf_speed_a == g.perf_speed_b) { app_toast("START = END"); break; }
                    bool fix = (gps_get()->fix_type >= 1 && gps_get()->has_pos);
                    if (!fix) { app_toast("NEED GPS FIX"); break; }
                    g.perf_state = 1; g.perf_time = 0; g.perf_trig_cnt = 0;
                    g.perf_speed0 = gps_get()->speed_kmh;
                    s_perf_prev_spd = g.perf_speed0;
                    app_save_settings();
                    app_toast(g.perf_speed_a < g.perf_speed_b ? "CONFIRM - ACCEL" : "CONFIRM - BRAKE");
                } else {
                    g.perf_edit = true;
                    g.perf_edit_field = g.perf_custom_focus;
                    g.perf_edit_old = g.perf_custom_focus == 0 ? g.perf_speed_a : g.perf_speed_b;
                    g.perf_edit_anim = 0;
                }
                return;
            }
            if (ev->button == BTN_BACK) app_back();
            break;
        }
        if (ev->button == BTN_UP && g.perf_state != 2) {
            g.entered = false;
            g.perf_state = 0;
            g.perf_time = 0;
            g.perf_mode_sel = g.perf_mode;
            app_toast("SELECT MODE");
            return;
        }
        if (ev->button == BTN_PUSH) {
            bool fix = (gps_get()->fix_type >= 1 && gps_get()->has_pos);
            if (g.perf_state == 0 || g.perf_state == 3) {
                if (!fix) { app_toast("NEED GPS FIX"); app_go(SC_GPS_DEBUG, 1); break; }
                g.perf_state = 1; g.perf_time = 0; g.perf_trig_cnt = 0;
                g.perf_speed0 = gps_get()->speed_kmh;
                s_perf_prev_spd = g.perf_speed0;
                bool accel = g.perf_mode == 0 ||
                    (g.perf_mode == 2 && g.perf_speed_a < g.perf_speed_b);
                app_toast(accel ? "ARMED - ACCEL" : "ARMED - BRAKE");
            } else {
                g.perf_state = 0; g.perf_time = 0; app_toast("CANCELLED");
            }
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_TRACK_EDIT:
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) { g.entered = true; }
            else if (ev->button == BTN_BACK) app_back();
        } else {
            if (d && g.ntracks) { g.sel = (g.sel + d + g.ntracks) % g.ntracks; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) app_toast("EDIT ON PHONE APP");
            else if (ev->button == BTN_BACK) app_back();
        }
        break;

    case SC_SETTINGS:
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_back();
        } else {
            if (g.set_edit) {
                if (ev->type == EV_PRESS || ev->type == EV_LONG || ev->type == EV_REPEAT) {
                    if (ev->button == BTN_LEFT || ev->button == BTN_RIGHT) {
                        int step = g.sel == 8 ? 5 : 10;
                        int delta = (ev->button == BTN_RIGHT ? step : -step);
                        if (g.sel == 0) {
                            g.set_brightness += delta;
                            if (g.set_brightness < 10) g.set_brightness = 10;
                            if (g.set_brightness > 100) g.set_brightness = 100;
                            lcd_set_brightness(g.set_brightness);
                        } else if (g.sel == 2) {
                            g.set_ledbright += delta;
                            if (g.set_ledbright < 0) g.set_ledbright = 0;
                            if (g.set_ledbright > 100) g.set_ledbright = 100;
                            ws2812_set_brightness(g.set_ledbright);
                        } else if (g.sel == 8) {
                            g.perf_trig_g100 += delta;
                            if (g.perf_trig_g100 < 5) g.perf_trig_g100 = 5;
                            if (g.perf_trig_g100 > 50) g.perf_trig_g100 = 50;
                        }
                        return;
                    }
                    if (ev->button == BTN_PUSH) {
                        g.set_edit = false;
                        lcd_set_brightness(g.set_brightness);
                        ws2812_set_brightness(g.set_ledbright);
                        app_save_settings();
                        app_toast("SETTING SAVED");
                        return;
                    }
                    if (ev->button == BTN_BACK) {
                        g.set_edit = false;
                        if (g.sel == 0) { g.set_brightness = g.set_edit_old; lcd_set_brightness(g.set_brightness); }
                        else if (g.sel == 2) { g.set_ledbright = g.set_edit_old; ws2812_set_brightness(g.set_ledbright); }
                        else if (g.sel == 8) g.perf_trig_g100 = g.set_edit_old;
                        app_toast("EDIT CANCELLED");
                        return;
                    }
                }
                return;
            }
            if (lr && (g.sel == 0 || g.sel == 2 || g.sel == 8)) {
                int step = g.sel == 8 ? 5 : 10;
                int delta = lr * step;
                if (g.sel == 0) {
                    g.set_brightness += delta;
                    if (g.set_brightness < 10) g.set_brightness = 10;
                    if (g.set_brightness > 100) g.set_brightness = 100;
                    lcd_set_brightness(g.set_brightness);
                } else if (g.sel == 2) {
                    g.set_ledbright += delta;
                    if (g.set_ledbright < 0) g.set_ledbright = 0;
                    if (g.set_ledbright > 100) g.set_ledbright = 100;
                    ws2812_set_brightness(g.set_ledbright);
                } else {
                    g.perf_trig_g100 += delta;
                    if (g.perf_trig_g100 < 5) g.perf_trig_g100 = 5;
                    if (g.perf_trig_g100 > 50) g.perf_trig_g100 = 50;
                }
                app_save_settings();
                g.settings_sel = g.sel;
                return;
            }
            if (d) { g.sel = (g.sel + d + 17) % 17; g.settings_sel = g.sel; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) {
                g.settings_sel = g.sel;
                if (g.sel == 0 || g.sel == 2 || g.sel == 8) {
                    g.set_edit = true;
                    g.set_edit_old = g.sel == 0 ? g.set_brightness :
                        (g.sel == 2 ? g.set_ledbright : g.perf_trig_g100);
                    app_toast(g.sel == 0 ? "BRIGHTNESS - L/R ADJUST" :
                              g.sel == 2 ? "LED BRIGHT - L/R ADJUST" : "LAUNCH G - L/R ADJUST");
                    return;
                }
                if (g.sel == 1) g.set_ledcolor = (g.set_ledcolor + 1) % 4;
                else if (g.sel == 3) { g.theme_light = !g.theme_light; theme_set(g.theme_light); }
                else if (g.sel == 4) app_go(SC_WIFI_SETUP, 1);
                else if (g.sel == 5) { g.set_bt = !g.set_bt; btmap_set_enabled(g.set_bt != 0); }
                else if (g.sel == 6) app_go(SC_SOUND, 1);
                else if (g.sel == 7) {
                    g.gps_rate_hz = g.gps_rate_hz == 1 ? 5 : g.gps_rate_hz == 5 ? 10 : g.gps_rate_hz == 10 ? 25 : 1;
                    app_toast(g.gps_rate_hz == 1 ? "GPS 1 HZ" : g.gps_rate_hz == 5 ? "GPS 5 HZ" :
                              g.gps_rate_hz == 10 ? "GPS 10 HZ" : "GPS 25 HZ");
                }
                else if (g.sel == 9) app_go(SC_IMU_DEBUG, 1);
                else if (g.sel == 10) app_go(SC_GPS_DEBUG, 1);
                else if (g.sel == 11) app_go(SC_LOCATION, 1);
                else if (g.sel == 12) {
                    g.ui_style = g.ui_style ? 0 : 1;
                    theme_set_style(g.ui_style);
                    theme_set(g.theme_light);
                    gfx_set_font(g.ui_style);
                    app_toast(g.ui_style ? "FORMULA 1 UI" : "PIXEL UI");
                }
                else if (g.sel == 13) app_go(SC_BTN_DEBUG, 1);
                else if (g.sel == 14) app_go(SC_FLASH, 1);
                else if (g.sel == 15) { cover_test_capture(); app_go(SC_COVER_TEST, 1); }
                else if (g.sel == 16) app_go(SC_COLOR_TUNE, 1);
                if (g.sel <= 5 || g.sel == 7 || g.sel == 12) app_save_settings();
            } else if (ev->button == BTN_BACK) app_back();
        }
        break;

    case SC_MODE_SELECT:
        if (d) { g.sel = (g.sel + d + 2) % 2; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) app_go(g.sel == 0 ? SC_NEW_TRACK_MARK_START : SC_TRACK_LIST, 1);
        else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_MOUNT_SELECT:
        if (d) { g.sel = (g.sel + d + 2) % 2; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            g.race_mount = g.sel;
            if (g.engine_valid) engine_set_profile(e, g.race_mount);
            app_go(g.raceTarget, 1);
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_NEW_TRACK_MARK_START:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (g.rec_state == 0) {
                if (gps_get()->fix_type < 2) { app_toast("NO GPS 3D FIX"); return; }
                app_rec_reset();
                if (!app_rec_reserve(REC_INITIAL)) { app_toast("REC BUFFER OOM"); return; }
                g.rec_state = 1; g.rec_has_ref = false;
                g.rec_line_ok = false; g.rec_armed = false; g.rec_along_prev = 0;
                g.rec_laps = 0; g.rec_last_lap = g.rec_best_lap = g.rec_delta = 0;
                g.rec_lap_time = 0;
                g.rec_last_fix_seq = 0; g.rec_prev_fix_us = 0; g.rec_lap_start_us = 0;
                app_toast("RECORDING");
            } else {
                if (g.rec_lap_n == 0) {
                    if (g.rec_n < 20 || g.rec_dist < 20.0f) {
                        app_toast("DRIVE A LAP FIRST"); return;
                    }
                    track_build_geo(&g.sessionTrack, g.rec_pts, g.rec_n,
                                    g.rec_lat0, g.rec_lon0, "NEW", 0);
                    g.sessionTrack_valid = true;
                    rec_capture_lap();
                }
                g.rec_state = 0;
                app_toast("SELECT LAPS TO BUILD");
                app_go(SC_REC_LAP_SELECT, 1);
            }
        } else if (ev->button == BTN_DOWN && g.rec_state == 0 && gps_get()->fix_type < 2) {
            app_go(SC_GPS_DEBUG, 1);     /* jump to GPS debug when there is no 3D fix */
        } else if (ev->button == BTN_BACK) {
            if (g.rec_state == 1) { g.rec_state = 0; app_toast("CANCELLED"); }
            else app_back();
        }
        break;

    case SC_RACING:
        if (ev->type == EV_PRESS && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
            g.panel = (g.panel + (ev->button == BTN_LEFT ? -1 : 1) + 4) % 4;
            return;
        }
        if (ev->button == BTN_BACK && ev->type == EV_LONG) {
            e->paused = true;
            race_store_end(e);
            app_record_race(0);
            app_push_confirm("SAVE THIS RECORD\nAS A NEW TRACK?", "YES", "NO", cb_racing_yes, cb_racing_no);
        }
        break;

    case SC_RACING_RECORDED:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) {
            e->paused = true;
            race_store_end(e);
            if (e->nLaps == 0) { app_back(); return; }
            app_push_confirm("KEEP RACE LOG?", "YES", "NO",
                             cb_save_race_log_yes, cb_save_race_log_no);
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
        } else if (ev->button == BTN_BACK) app_back();
        break;
    }

    case SC_SECTION_MARKING:
        if (lr) {
            float step = (ev->type == EV_REPEAT || ev->type == EV_LONG) ? 0.05f : 0.02f;
            g.sec_pos = u_clampf(g.sec_pos + lr * step, 0.01f, 0.99f);
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
            app_back();
        }
        break;

    case SC_VEHICLE_SELECT:
        if (d) { g.sel = (g.sel + d + 4) % 4; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) { g.sessionVehicle = g.sel; app_go(SC_TRACK_NAMING, 1); }
        else if (ev->button == BTN_BACK) app_back();
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
            bool stored = false;
            int slot = tstore_first_free();
            if (slot >= 0) stored = tstore_save(slot, t);
            t->store_slot = stored ? slot : -1;
            if (g.ntracks < MAX_TRACKS) g.tracks[g.ntracks++] = *t;
            app_toast(stored ? "TRACK SAVED" : "SAVED TO RAM ONLY");
            g.sessionTrack_valid = false;
            g.session_nsec = 0;
            if (g.rec_lap_n > 0 && rec_selected_count() > 0) {
                app_push_confirm("KEEP RACE LOG?", "YES", "NO",
                                 cb_keep_log_yes, cb_keep_log_no);
            } else {
                app_go_home_to(0);
            }
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_REC_LAP_SELECT: {
        int total = g.rec_lap_n + 1;
        if (d) { g.rec_sel_cursor = (g.rec_sel_cursor + d + total) % total; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (g.rec_sel_cursor < g.rec_lap_n) {
                g.rec_lap_sel ^= (1u << g.rec_sel_cursor);
            } else {
                if (rec_selected_count() == 0) { app_toast("SELECT AT LEAST 1 LAP"); return; }
                app_push_confirm("BUILD TRACK FROM SELECTED LAPS?", "BUILD", "CANCEL",
                                 cb_build_fused_track, NULL);
            }
        } else if (ev->button == BTN_BACK) app_back();
        break;
    }

    case SC_REC_PREVIEW:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            app_push_confirm("CONFIRM FUSED TRACK?", "CONFIRM", "REDO",
                             cb_confirm_fused_track, NULL);
        } else if (ev->button == BTN_BACK) app_back();
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
            g.raceBack = t->geo ? SC_TRACK_START : SC_TRACK_LIST;
            app_go(t->geo ? SC_TRACK_START : SC_MOUNT_SELECT, 1);
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_TRACK_START: {
        if (d && ev->type == EV_PRESS) {
            app_push_confirm("SWITCH TRACK DIRECTION?", "YES", "NO",
                             cb_track_reverse_yes, NULL);
            return;
        }
        if (lr) {
            g.start_f += lr * 0.004f;
            g.start_f = fmodf(g.start_f + 1.0f, 1.0f);
            return;
        }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            track_t *t = g.engine.track;
            if (t && t->geo && t->nscale > 0) {
                pt_t q = track_point_at(t, g.start_f);
                float mx = (q.x - t->nox) / t->nscale;
                float my = (q.y - t->noy) / t->nscale;
                geo_ref_t r = { t->glat0, t->glon0 };
                double lat, lon;
                geo_from_meters(&r, mx, my, &lat, &lon);
                pt_t a = track_point_at(t, g.start_f - 0.004f);
                pt_t b = track_point_at(t, g.start_f + 0.004f);
                t->line_lat = lat;
                t->line_lon = lon;
                t->line_hdg = atan2f(b.y - a.y, b.x - a.x);
                if (t->reverse) t->line_hdg += (float)M_PI;
                if (t->store_slot >= 0) tstore_save(t->store_slot, t);
                app_toast("START LINE SAVED");
            }
            app_go(SC_MOUNT_SELECT, 1);
        } else if (ev->button == BTN_BACK) app_back();
        break;
    }

    case SC_HISTORY:
        if (!g.entered) {
            if (lr) { app_go_home(lr); return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH) g.entered = true;
            else if (ev->button == BTN_BACK) app_back();
        } else {
            if (d && g.nlog) { g.sel = (g.sel + d + g.nlog) % g.nlog; return; }
            if (ev->type != EV_PRESS) return;
            if (ev->button == BTN_PUSH && g.nlog) { g.hist_rec = g.nlog - 1 - g.sel; app_go(SC_HISTORY_DETAIL, 1); }
            else if (ev->button == BTN_BACK) app_back();
        }
        break;

    case SC_HISTORY_DETAIL:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) { app_back(); return; }
        if (d && g.hist_rec >= 0 && g.hist_rec < g.nlog) {
            int n = g.log[g.hist_rec].lapCount;
            if (n) g.sel = (g.sel + d + n) % n;
            return;
        }
        break;

    case SC_FLASH:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) app_back();
        break;

    case SC_COVER_TEST:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) app_back();
        break;

    case SC_COLOR_TUNE:
        if (d) { g.sel = (g.sel + d + 2) % 2; return; }
        if ((ev->type == EV_PRESS || ev->type == EV_REPEAT) &&
            (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
            int step = ev->button == BTN_RIGHT ? 5 : -5;
            if (g.sel == 0) {
                g.media_black_pct += step;
                if (g.media_black_pct < 0) g.media_black_pct = 0;
                if (g.media_black_pct > 100) g.media_black_pct = 100;
            } else {
                g.media_near_black += step;
                if (g.media_near_black < 0) g.media_near_black = 0;
                if (g.media_near_black > 100) g.media_near_black = 100;
            }
            btmap_set_color_tuning(g.media_near_black, g.media_black_pct);
            return;
        }
        if (ev->type == EV_PRESS && ev->button == BTN_PUSH) {
            app_save_settings();
            app_toast("COLOR SAVED");
        } else if (ev->type == EV_PRESS && ev->button == BTN_BACK) {
            app_save_settings();
            app_back();
        }
        break;

    case SC_BTN_DEBUG:
        if (ev->type == EV_PRESS && ev->button == BTN_BACK) app_back();
        break;

    case SC_GPS_DEBUG:
        if (ev->type == EV_PRESS && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
            g.dbg_page = (g.dbg_page + 1) % 2;   /* switch debug sub-page */
            return;
        }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_BACK) app_back();
        break;

    case SC_LOCATION:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_BACK) app_back();
        break;

    case SC_IMU_DEBUG:
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            g.i_ox += g.i_ax; g.i_oy += g.i_ay; g.i_oz += g.i_az;
            g.i_ogx += g.i_gx; g.i_ogy += g.i_gy; g.i_ogz += g.i_gz;
            g.i_gmax = 0;
            app_toast("IMU TARE");
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_SOUND:
        if (lr) {
            if (g.sel == 0) {
                g.set_sound_vol += lr * 10;
                if (g.set_sound_vol < 0) g.set_sound_vol = 0;
                if (g.set_sound_vol > 100) g.set_sound_vol = 100;
                buzzer_set_volume(g.set_sound_vol);
                if (g.set_sound_vol > 0) buzzer_play_id(SND_CONFIRM);
                app_save_settings();
            }
            return;
        }
        if (d) { g.sel = (g.sel + d + 5) % 5; return; }
        if (ev->type != EV_PRESS) return;
        if (ev->button == BTN_PUSH) {
            if (g.sel == 4) {
                buzzer_play_id(SND_MAX_DUDUDU);
                app_toast("MAX DUDUDU");
                return;
            }
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
        } else if (ev->button == BTN_BACK) app_back();
        break;

    case SC_IMU_CALIB:
        if (ev->type != EV_PRESS && ev->type != EV_DOUBLE) return;
        if (ev->type == EV_PRESS && ev->button == BTN_PUSH) {
            if (!g.imu_stable || g.imu_ns < 8) {
                app_toast("HOLD STILL");
                break;
            }
            imu6_t m = {0, 0, 0, 0, 0, 0};
            int n2 = g.imu_ns;
            for (int i = 0; i < n2; i++) {
                m.ax += g.imu_samples[i].ax; m.ay += g.imu_samples[i].ay; m.az += g.imu_samples[i].az;
                m.gx += g.imu_samples[i].gx; m.gy += g.imu_samples[i].gy; m.gz += g.imu_samples[i].gz;
            }
            m.ax /= n2; m.ay /= n2; m.az /= n2; m.gx /= n2; m.gy /= n2; m.gz /= n2;
            app_apply_imu_reference(&m);
            g.i_gmax = 0;
            g.imu_phase = 1; g.imu_doneTimer = 0.8f; g.imu_progress = IMU_CALIB_MS;
            app_toast("IMU CALIBRATED");
            if (g.set_snd_calib) buzzer_play_id(SND_CALIB);
        } else if (ev->type == EV_DOUBLE && ev->button == BTN_PUSH) {   /* 2x push = resample */
            g.imu_ns = 0; g.imu_progress = 0; g.imu_phase = 0; g.imu_stable = false;
            imu_win_reset();
            mpu_clear_calibration(&g.imu);
            app_toast("RESAMPLING");
        } else if (ev->button == BTN_BACK) {
            app_toast("IMU SKIPPED");
            app_go_home_to(0);
        }
        break;

    default: break;
    }
}

/* racing panel switching shared by RACING and RACING_RECORDED */
void screens_handle_racing_common(const event_t *ev) {
    if (ev->type == EV_PRESS && (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
        g.panel = (g.panel + (ev->button == BTN_LEFT ? -1 : 1) + 4) % 4;
        return;
    }
    if (ev->button == BTN_BACK && ev->type == EV_LONG) {
        g.engine.paused = true;
        race_store_end(&g.engine);
        if (g.engine.nLaps == 0) { app_back(); return; }
        app_push_confirm("KEEP RACE LOG?", "YES", "NO",
                         cb_save_race_log_yes, cb_save_race_log_no);
    }
}

/* ================= chrome ================= */
void screens_chrome(void) {
    char b[80];
    g.statusColor = PAL.dim;
    for (int i = 0; i < LED_COUNT; i++) g.leds[i] = 0;
    switch (g.cur) {
    case SC_HOME: snprintf(g.status, sizeof g.status, "MAIN MENU"); break;
    case SC_RACE_MENU: snprintf(g.status, sizeof g.status, "RACE MENU"); break;
    case SC_NAV_MENU: snprintf(g.status, sizeof g.status, "NAVIGATION MENU"); break;
    case SC_PERF: {
        char mode[24];
        if (g.perf_mode == 0) strcpy(mode, "0-100");
        else if (g.perf_mode == 1) strcpy(mode, "100-0");
        else snprintf(mode, sizeof mode, "%d-%d", g.perf_speed_a, g.perf_speed_b);
        snprintf(g.status, sizeof g.status, "PERF %s  %s", mode,
                 g.perf_state == 0 ? "READY" : g.perf_state == 1 ? "ARMED" : g.perf_state == 2 ? "RUNNING" : "DONE");
        break;
    }
    case SC_NAV:
        snprintf(g.status, sizeof g.status, "LOCATION  %.0f KM/H  %s  %s",
                 gps_get()->speed_kmh, netcfg_connected() ? "WIFI" : "NO WIFI",
                 nav_status() == 1 ? "MAP OK" : "MAP...");
        break;
    case SC_PHONE:
        snprintf(g.status, sizeof g.status, "PHONE  %s  FRAMES %d", btmap_state(), btmap_frames());
        g.statusColor = btmap_connected() ? PAL.green : PAL.dim;
        break;
    case SC_MEDIA:
        snprintf(g.status, sizeof g.status, "MEDIA  %s  %s", btmap_state(),
                 btmap_media_playing() ? "PLAYING" : "PAUSED");
        g.statusColor = btmap_subscribed() ? PAL.green : PAL.dim;
        break;
    case SC_WIFI_SETUP: {
        char st[48]; netcfg_status(st, sizeof st);
        snprintf(g.status, sizeof g.status, "WIFI  %s", st);
        g.statusColor = netcfg_connected() ? PAL.green : PAL.yellow;
        break;
    }
    case SC_TRACK_EDIT: snprintf(g.status, sizeof g.status, g.entered ? "TRACK EDIT - EDITING" : "TRACK EDIT - PUSH TO EDIT"); break;
    case SC_SETTINGS: snprintf(g.status, sizeof g.status, g.entered ? "SETTINGS - ADJUST" : "SETTINGS - PUSH TO ENTER"); break;
    case SC_FLASH: snprintf(g.status, sizeof g.status, "FLASH MAP"); break;
    case SC_COVER_TEST: snprintf(g.status, sizeof g.status, "STATIC COVER TEST"); break;
    case SC_COLOR_TUNE: snprintf(g.status, sizeof g.status, "MEDIA COLOR TUNING"); break;
    case SC_MODE_SELECT: snprintf(g.status, sizeof g.status, "MODE SELECT"); break;
    case SC_MOUNT_SELECT: snprintf(g.status, sizeof g.status, g.sel == 0 ? "MOUNT - FIXED (TRUST IMU)" : "MOUNT - NOT FIXED (TRUST GPS)"); break;
    case SC_REC_LAP_SELECT:
        snprintf(g.status, sizeof g.status, "SELECT %d LAPS", rec_selected_count());
        g.statusColor = PAL.cyan; break;
    case SC_REC_PREVIEW:
        snprintf(g.status, sizeof g.status, "FUSED TRACK PREVIEW");
        g.statusColor = PAL.cyan; break;
    case SC_NEW_TRACK_MARK_START:
        if (g.rec_state == 1) {
            char tb[24]; u_fmt_time(tb, sizeof tb, g.rec_lap_time);
            snprintf(g.status, sizeof g.status, "REC LAP %d  %s  %.0f M", g.rec_laps + 1, tb, g.rec_dist);
        } else snprintf(g.status, sizeof g.status, gps_get()->fix_type >= 2 ? "NEW TRACK - PUSH AT START" : "NEW TRACK - WAIT 3D FIX");
        break;
    case SC_RACING:
    case SC_RACING_RECORDED: {
        engine_t *e = &g.engine;
        u_fmt_time(b, sizeof b, e->elapsed);
        snprintf(g.status, sizeof g.status, "%s %d/4   %s %d   %s", RACE_PANELS[g.panel], g.panel + 1,
                 e->outlap_pending ? "OUTLAP" : "LAP", e->lap, b);
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
    case SC_IMU_CALIB: snprintf(g.status, sizeof g.status, "IMU CALIB - %s - %d%%", g.imu_phase ? "DONE" : (g.imu_stable ? "STABLE" : "MOVING"), imu_pct());
        g.statusColor = g.imu_stable ? PAL.green : PAL.yellow; break;
    case SC_LOCATION: {
        gps_state_t *gp = gps_get();
        snprintf(g.status, sizeof g.status, "CHINA MAP  FIX %s  SAT %d  HDOP %.1f",
                 gp->fix_type == 2 ? "3D" : (gp->fix_type == 1 ? "2D" : "NO"), gp->sats_used, gp->hdop);
        g.statusColor = gp->fix_type >= 1 ? PAL.green : PAL.orange;
        break;
    }
    case SC_SOUND:
        snprintf(g.status, sizeof g.status, "SOUND  VOL %d%%  %s", g.set_sound_vol,
                 g.set_sound_vol ? "ON" : "MUTED");
        g.statusColor = g.set_sound_vol ? PAL.green : PAL.dim;
        break;
    case SC_BTN_DEBUG:
        snprintf(g.status, sizeof g.status, "BUTTON DEBUG");
        g.statusColor = PAL.cyan; break;
    case SC_IMU_DEBUG:
        snprintf(g.status, sizeof g.status, "IMU DEBUG  MPU6050 %s", g.imu_ok ? "OK" : "NOT FOUND");
        g.statusColor = g.imu_ok ? PAL.green : PAL.red;
        break;
    case SC_TRACK_START:
        snprintf(g.status, sizeof g.status, "START LINE  %.1f%%  PUSH SAVE", g.start_f * 100.0f);
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
