#include "app.h"
#include "gfx.h"
#include "lcd.h"
#include "util.h"
#include "battery.h"
#include "gps.h"
#include "buzzer.h"
#include "mpu6050.h"
#include "ws2812.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "math.h"
#include "string.h"
#include <stdio.h>
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

app_t g;

static const scid_t HOME_PAGES[4] = {SC_HOME, SC_HISTORY, SC_TRACK_EDIT, SC_SETTINGS};

/* ================= records ================= */
static void section_best_from_engine(const engine_t *e, histrec_t *r) {
    int nsec = r->nsec;
    for (int i = 0; i < nsec; i++) { r->secValid[i] = false; r->secBest[i] = 0; r->secBestLap[i] = 0; }
    if (!nsec) return;
    for (int li = 0; li < e->nLaps; li++) {
        const lap_t *l = &e->laps[li];
        float prev = 0;
        for (int si = 0; si < nsec; si++) {
            float f = r->sections[si];
            int iEnd = (int)floorf(f * TRK_N); if (iEnd > TRK_N - 1) iEnd = TRK_N - 1;
            int iStart = (prev == 0) ? 0 : (int)floorf(prev * TRK_N);
            if (iStart > TRK_N - 1) iStart = TRK_N - 1;
            float tE = l->times[iEnd];
            float tS = (prev == 0) ? 0 : l->times[iStart];
            if (!isnan(tE) && !isnan(tS)) {
                float t = tE - tS;
                if (t >= 0 && (!r->secValid[si] || t < r->secBest[si])) {
                    r->secValid[si] = true; r->secBest[si] = t; r->secBestLap[si] = l->index;
                }
            }
            prev = f;
        }
    }
}

static void record_from_engine(const char *date, float duration, int mode) {
    if (g.engine.nLaps == 0) return;
    if (g.nlog >= HISTORY_RECORDS) {
        memmove(&g.log[0], &g.log[1], sizeof(histrec_t) * (HISTORY_RECORDS - 1));
        g.nlog = HISTORY_RECORDS - 1;
    }
    histrec_t *r = &g.log[g.nlog++];
    memset(r, 0, sizeof(*r));
    strncpy(r->trackName, g.engine.track->name, sizeof(r->trackName) - 1);
    strncpy(r->date, date, sizeof(r->date) - 1);
    r->mode = mode;
    r->duration = duration;
    r->lapCount = g.engine.nLaps;
    r->vehicle = g.engine.track->vehicle;
    r->hasBest = g.engine.hasBestLap;
    r->bestTime = g.engine.bestTime;
    r->bestIndex = g.engine.bestIndex;
    for (int i = 0; i < g.engine.nLaps && i < HISTORY_LAPS; i++) {
        r->lapTime[i] = g.engine.laps[i].time;
        r->lapIndex[i] = g.engine.laps[i].index;
    }
    r->nsec = g.engine.track->nsec;
    for (int i = 0; i < r->nsec; i++) r->sections[i] = g.engine.track->sections[i];
    section_best_from_engine(&g.engine, r);
    const lap_t *best = NULL;
    if (g.engine.hasBestLap && g.engine.bestIndex >= 1) best = &g.engine.laps[g.engine.bestIndex - 1];
    else best = &g.engine.laps[0];
    r->nTrace = best->nTrace;
    memcpy(r->trace, best->trace, sizeof(pt_t) * best->nTrace);
}

void app_record_race(int mode) {
    if (!g.engine_valid || g.engine.logged) return;
    g.engine.logged = true;
    char date[20];
    snprintf(date, sizeof(date), "REC %02d", g.nlog + 1);
    record_from_engine(date, g.engine.time, mode);
}

/* ================= seed (js/app.js seedTracks/seedHistory) ================= */
static void seed_tracks(void) {
    for (int i = 0; i < TRACK_DEF_COUNT; i++) track_build_def(&g.tracks[i], i);
    /* sections from seedTracks() */
    g.tracks[0].nsec = 3; g.tracks[0].sections[0] = 0.26f; g.tracks[0].sections[1] = 0.55f; g.tracks[0].sections[2] = 0.8f;
    g.tracks[1].nsec = 2; g.tracks[1].sections[0] = 0.4f;  g.tracks[1].sections[1] = 0.72f;
    g.tracks[2].nsec = 2; g.tracks[2].sections[0] = 0.33f; g.tracks[2].sections[1] = 0.66f;
    g.ntracks = 1;   /* keep a single simulated sample track; real tracks are recorded */
}

static void seed_history(void) {
    struct { int track; int laps; float dur; const char *date; } demo[3] = {
        {0, 7, 600, "09-27 18:20"},
        {1, 5, 420, "09-27 17:05"},
        {2, 4, 300, "09-26 20:41"},
    };
    for (int i = 0; i < 3; i++) {
        track_t *t = &g.tracks[demo[i].track];
        engine_init(&g.engine, t, true, &g.imu);
        engine_set_profile(&g.engine, 0);
        g.engine.logged = false;
        int guard = 0;
        while (g.engine.nLaps < demo[i].laps && guard++ < 200000)
            engine_update(&g.engine, 0.05f);
        record_from_engine(demo[i].date, demo[i].dur, 0);
    }
    g.engine_valid = false;
}

/* ================= navigation ================= */
void app_go(scid_t id, int dir) {
    g.prev_cur = g.cur;
    g.cur = id;
    g.dir = dir;
    g.screenT = 0;
    screens_enter(id);
}

void app_go_home(int delta) {
    int n = 4;
    g.homeIndex = ((g.homeIndex + delta) % n + n) % n;
    app_go(HOME_PAGES[g.homeIndex], delta > 0 ? 1 : -1);
}
void app_go_home_to(int idx) {
    int dir = idx < g.homeIndex ? -1 : 1;
    g.homeIndex = idx;
    app_go(HOME_PAGES[idx], dir);
}

void app_toast(const char *msg) {
    strncpy(g.toastMsg, msg, sizeof(g.toastMsg) - 1);
    g.toastMsg[sizeof(g.toastMsg) - 1] = 0;
    g.toastT = 1.4f;
}

/* ================= overlay (confirm modal) ================= */
void app_push_confirm(const char *text, const char *yes, const char *no,
                      action_fn on_yes, action_fn on_no) {
    if (g.set_snd_confirm) buzzer_play_id(SND_CONFIRM);
    g.overlay_active = true;
    g.overlay = SC_CONFIRM;
    strncpy(g.confirm_text, text, sizeof(g.confirm_text) - 1);
    g.confirm_text[sizeof(g.confirm_text) - 1] = 0;
    strncpy(g.confirm_yes, yes ? yes : "YES", sizeof(g.confirm_yes) - 1);
    strncpy(g.confirm_no, no ? no : "NO", sizeof(g.confirm_no) - 1);
    g.confirm_sel = 0;
    g.confirm_on_yes = on_yes;
    g.confirm_on_no = on_no;
}

void app_pop_overlay(void) {
    g.overlay_active = false;
    g.overlay = SC_COUNT;
}

static void confirm_choose(bool yes) {
    action_fn cb = yes ? g.confirm_on_yes : g.confirm_on_no;
    app_pop_overlay();
    if (cb) cb();
}

/* ================= event dispatch ================= */
static void confirm_handle(const event_t *ev) {
    if ((ev->type == EV_PRESS || ev->type == EV_LONG || ev->type == EV_REPEAT)) {
        if (ev->button == BTN_UP || ev->button == BTN_DOWN) {
            g.confirm_sel = (g.confirm_sel + (ev->button == BTN_DOWN ? 1 : -1) + 2) % 2;
            return;
        }
    }
    if (ev->type != EV_PRESS && ev->type != EV_DOUBLE) return;
    if (ev->button == BTN_PUSH) confirm_choose(g.confirm_sel == 0);
    else if (ev->button == BTN_BACK) confirm_choose(false);
}

void app_dispatch(const event_t *ev) {
    if (g.overlay_active) confirm_handle(ev);
    else screens_handle(ev);
}

void app_update(float dt) {
    g.now += dt * 1000.0f;
    g.screenT += dt;
    if (g.toastT > 0) g.toastT -= dt;
    /* chime when GPS reaches a 3D fix */
    int fx = gps_get()->fix_type;
    if (fx >= 2 && g.prev_fix < 2 && g.set_snd_fix) buzzer_play_id(SND_FIX);
    g.prev_fix = fx;
    gnss_update(&g.gnss, dt);
    screens_update(dt);
}

/* ================= chrome ================= */
static void render_led_bar(void) {
    gfx_fill(0, 0, LCD_W, LED_H, PAL.devicebg);
    const int gap = 3, h = 11, y = (LED_H - h) / 2;
    int pad = 84;                                  /* leave the top-left for the clock */
    int w = (LCD_W - pad - 6 - 9 * gap) / LED_COUNT;
    for (int i = 0; i < LED_COUNT; i++) {
        uint16_t c = PAL.ledoff;
        if (g.leds[i] == 1) c = PAL.red;
        else if (g.leds[i] == 2) c = PAL.green;
        gfx_fill(pad + i * (w + gap), y, w, h, c);
    }
}

/* top-left clock: GPS UTC time + 8h (China time), shown on every screen */
static void render_clock(void) {
    gps_state_t *gp = gps_get();
    char t[12];
    if (!gp->has_time) {
        gfx_text(4, (LED_H - 11) / 2, "--:--:--", PAL.dim, 1);
        return;
    }
    int hh = gp->utc_h + 8;
    if (hh >= 24) hh -= 24;
    snprintf(t, sizeof t, "%02d:%02d:%02d", hh, gp->utc_m, gp->utc_s);
    gfx_text(4, (LED_H - 11) / 2, t, PAL.cyan, 1);
}

static void render_status(void) {
    gfx_fill(0, STATUS_Y, LCD_W, STATUS_H, PAL.statusbg);
    gfx_text(6, STATUS_Y + (STATUS_H - 7) / 2, g.status, g.statusColor, 1);
}

static void draw_multiline_center(int cx, int y, const char *s, uint16_t c, int scale, int lineH) {
    char line[64];
    int n = 0;
    for (const char *p = s;; p++) {
        if (*p == '\n' || *p == 0) {
            line[n] = 0;
            gfx_text_center(cx, y, line, c, scale);
            y += lineH;
            n = 0;
            if (*p == 0) break;
        } else if (n < 63) {
            line[n++] = *p;
        }
    }
}

static void hsv2rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b) {
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rr, gg, bb;
    if (h < 60) { rr = c; gg = x; bb = 0; }
    else if (h < 120) { rr = x; gg = c; bb = 0; }
    else if (h < 180) { rr = 0; gg = c; bb = x; }
    else if (h < 240) { rr = 0; gg = x; bb = c; }
    else if (h < 300) { rr = x; gg = 0; bb = c; }
    else { rr = c; gg = 0; bb = x; }
    *r = (uint8_t)((rr + m) * 255);
    *g = (uint8_t)((gg + m) * 255);
    *b = (uint8_t)((bb + m) * 255);
}

/* per-screen WS2812 effects, kept in sync with the on-screen LED bar */
static void ledfx_update(void) {
    ws2812_set_brightness(g.set_ledbright);
    for (int i = 0; i < LED_COUNT; i++) ws2812_set(i, 0, 0, 0);

    switch (g.cur) {
    case SC_IMU_CALIB: {
        /* boot progress bar: light the LEDs one by one as calibration advances */
        float p = g.imu_progress / IMU_CALIB_MS;
        if (p < 0) p = 0; if (p > 1) p = 1;
        if (g.imu_phase == 1) p = 1;
        int lit = (int)(p * LED_COUNT + 0.5f);
        for (int i = 0; i < LED_COUNT; i++) {
            if (i < lit) ws2812_set(i, 255, 255, 255);
            else         ws2812_set(i, 0, 0, 0);
        }
        break;
    }
    case SC_HOME: {
        /* colourful running rainbow marquee */
        float base = g.now / 1000.0f * 140.0f;   /* scroll speed */
        for (int i = 0; i < LED_COUNT; i++) {
            float hue = base + i * 36.0f;
            uint8_t r, gg, b;
            hsv2rgb(hue, 1.0f, 1.0f, &r, &gg, &b);
            ws2812_set(i, r, gg, b);
        }
        break;
    }
    case SC_RACING:
    case SC_RACING_RECORDED:
        for (int i = 0; i < LED_COUNT; i++) {
            if (g.leds[i] == 1) ws2812_set(i, 255, 0, 0);       /* slower -> red */
            else if (g.leds[i] == 2) ws2812_set(i, 0, 255, 0);  /* faster -> green */
        }
        break;
    default: break;
    }
    ws2812_show();
}

static void render_battery(void) {
    int mv = battery_mv();
    if (mv <= 0) return;
    char v[12], s[24];
    u_fmt_f(v, sizeof v, mv / 1000.0f, 2);
    int pct = (mv - 3300) * 100 / (4200 - 3300);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(s, sizeof s, "%sV %d%%", v, pct);
    int w = gfx_text_w(s, 0) + 6;
    int x = LCD_W - 4 - w, y = CONTENT_Y + 1;
    gfx_pxbox(x, y, w, 12, PAL.line, 1, PAL.panel);
    uint16_t c = mv < 3400 ? PAL.red : (mv < 3600 ? PAL.orange : PAL.green);
    gfx_text(x + 3, y + 3, s, c, 0);
}

static void render_toast(void) {
    if (g.toastT <= 0 || !g.toastMsg[0]) return;
    int w = gfx_text_w(g.toastMsg, 1) + 16;
    if (w > LCD_W - 20) w = LCD_W - 20;
    int x = (LCD_W - w) / 2;
    int h = 18, y = STATUS_Y - h - 6;
    gfx_pxbox(x, y, w, h, PAL.cyan, 1, PAL.surface);
    gfx_text_center(LCD_W / 2, y + 6, g.toastMsg, PAL.white, 1);
}

static void render_overlay(void) {
    gfx_reset_view();
    gfx_dim(0, CONTENT_Y, LCD_W, CONTENT_H, PAL.devicebg);
    int w = 250, h = 108;
    int x = (LCD_W - w) / 2, y = CONTENT_Y + (CONTENT_H - h) / 2;
    gfx_pxbox(x, y, w, h, PAL.cyan, 1, PAL.panel);
    draw_multiline_center(LCD_W / 2, y + 16, g.confirm_text, PAL.white, 1, 12);
    int bw = 90, bh = 26, by = y + h - bh - 12;
    int bx1 = LCD_W / 2 - bw - 8, bx2 = LCD_W / 2 + 8;
    uint16_t b1 = (g.confirm_sel == 0) ? PAL.cyan : PAL.line;
    uint16_t b2 = (g.confirm_sel == 1) ? PAL.cyan : PAL.line;
    gfx_pxbox(bx1, by, bw, bh, b1, 1, (g.confirm_sel == 0) ? PAL.seltint : PAL.panel);
    gfx_pxbox(bx2, by, bw, bh, b2, 1, (g.confirm_sel == 1) ? PAL.seltint : PAL.panel);
    gfx_text_center(bx1 + bw / 2, by + (bh - 7) / 2, g.confirm_yes, (g.confirm_sel == 0) ? PAL.cyan : PAL.dim, 1);
    gfx_text_center(bx2 + bw / 2, by + (bh - 7) / 2, g.confirm_no, (g.confirm_sel == 1) ? PAL.cyan : PAL.dim, 1);
}

void app_render(void) {
    gfx_reset_view();
    gfx_clear(PAL.stage);

    /* content: screens are written in 320x200 content-local coordinates */
    gfx_set_offset(0, CONTENT_Y);
    gfx_set_scale(1.0f);
    gfx_set_clip(0, CONTENT_Y, LCD_W, CONTENT_H);
    screens_render();

    /* overlay on top of content */
    if (g.overlay_active) render_overlay();

    /* chrome */
    gfx_reset_view();
    screens_chrome();
    render_led_bar();
    render_clock();
    render_status();
    render_battery();
    render_toast();
    ledfx_update();
}

/* ================= persistent settings (NVS) ================= */
static void settings_load(void) {
    nvs_handle_t h;
    if (nvs_open("rb", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "theme", &v) == ESP_OK) g.theme_light = v ? 1 : 0;
    if (nvs_get_i32(h, "bl", &v) == ESP_OK) g.set_brightness = v;
    if (nvs_get_i32(h, "ledcol", &v) == ESP_OK) g.set_ledcolor = v;
    if (nvs_get_i32(h, "ledbr", &v) == ESP_OK) g.set_ledbright = v;
    if (nvs_get_i32(h, "wifi", &v) == ESP_OK) g.set_wifi = v ? 1 : 0;
    if (nvs_get_i32(h, "bt", &v) == ESP_OK) g.set_bt = v ? 1 : 0;
    if (nvs_get_i32(h, "sndvol", &v) == ESP_OK) g.set_sound_vol = v;
    if (nvs_get_i32(h, "sndcfm", &v) == ESP_OK) g.set_snd_confirm = v ? 1 : 0;
    if (nvs_get_i32(h, "sndcal", &v) == ESP_OK) g.set_snd_calib = v ? 1 : 0;
    if (nvs_get_i32(h, "sndfix", &v) == ESP_OK) g.set_snd_fix = v ? 1 : 0;
    if (nvs_get_i32(h, "gpshz", &v) == ESP_OK) g.gps_rate_hz = v;
    nvs_close(h);
}

void app_save_settings(void) {
    nvs_handle_t h;
    if (nvs_open("rb", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "theme", g.theme_light);
    nvs_set_i32(h, "bl", g.set_brightness);
    nvs_set_i32(h, "ledcol", g.set_ledcolor);
    nvs_set_i32(h, "ledbr", g.set_ledbright);
    nvs_set_i32(h, "wifi", g.set_wifi);
    nvs_set_i32(h, "bt", g.set_bt);
    nvs_set_i32(h, "sndvol", g.set_sound_vol);
    nvs_set_i32(h, "sndcfm", g.set_snd_confirm);
    nvs_set_i32(h, "sndcal", g.set_snd_calib);
    nvs_set_i32(h, "sndfix", g.set_snd_fix);
    nvs_set_i32(h, "gpshz", g.gps_rate_hz);
    nvs_commit(h);
    nvs_close(h);
}

/* ================= boot ================= */
void app_init(void) {
    memset(&g, 0, sizeof(g));
    g.seed = esp_random();
    u_srand(g.seed);
    g.simSpeed = 1.0f;
    g.set_brightness = START_BRIGHTNESS;
    g.set_ledcolor = 0;
    g.set_ledbright = START_LED_BRIGHT;
    g.set_sound_vol = START_SOUND_VOL;
    g.set_snd_confirm = true; g.set_snd_calib = true; g.set_snd_fix = true;
    g.gps_rate_hz = 10;
    g.theme_light = START_THEME_LIGHT ? 1 : 0;
    /* load persisted settings */
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    theme_set(g.theme_light);
    lcd_set_brightness(g.set_brightness);
    ws2812_init();
    ws2812_set_brightness(g.set_ledbright);
    buzzer_set_volume(g.set_sound_vol);
    mpu_init(&g.imu);
    g.imu_ok = mpu6050_init();
    if (g.imu_ok) {
        /* Treat the boot pose as the IMU zero point: average a few samples and
         * keep them as the offset. This removes gravity + mounting angle, so the
         * live values become driving G (0 at rest, whatever the board is tilted). */
        float sx = 0, sy = 0, sz = 0, rx = 0, ry = 0, rz = 0, tp;
        int n = 0;
        for (int i = 0; i < 16; i++) {
            float ax, ay, az, gx, gy, gz;
            if (mpu6050_read(&ax, &ay, &az, &gx, &gy, &gz, &tp)) {
                sx += ax; sy += ay; sz += az; rx += gx; ry += gy; rz += gz; n++;
            }
            vTaskDelay(pdMS_TO_TICKS(6));
        }
        if (n) {
            g.i_ox = sx / n; g.i_oy = sy / n; g.i_oz = sz / n;
            g.i_ogx = rx / n; g.i_ogy = ry / n; g.i_ogz = rz / n;
            /* the axis carrying gravity is the vertical one (board mounted
             * upright); racing G is measured in the other two axes */
            float am = fabsf(g.i_ox); g.i_vaxis = 0;
            if (fabsf(g.i_oy) > am) { am = fabsf(g.i_oy); g.i_vaxis = 1; }
            if (fabsf(g.i_oz) > am) { am = fabsf(g.i_oz); g.i_vaxis = 2; }
            ESP_LOGI("imu", "boot zero: a(%d,%d,%d)mg  r(%d,%d,%d)mdps  vaxis=%d",
                     (int)(g.i_ox * 1000), (int)(g.i_oy * 1000), (int)(g.i_oz * 1000),
                     (int)(g.i_ogx * 1000), (int)(g.i_ogy * 1000), (int)(g.i_ogz * 1000),
                     g.i_vaxis);
        }
    }
    gnss_init(&g.gnss);
    seed_tracks();
    seed_history();
    gps_set_rate(g.gps_rate_hz);   /* find the module and program its output rate */
    app_go(SC_IMU_CALIB, 1);
}

/* ================= autoplay demo ================= */
#if !BTN_ENABLE
typedef struct { btn_id_t btn; float releaseAt; bool active; } pending_t;
static pending_t s_auto;
static scid_t s_auto_last = SC_COUNT;
static float s_auto_t = 0;
static int s_auto_step = 0;
static int s_settings_tours = 0;
static bool s_hist_visited = false;

static void auto_release_check(void) {
    if (s_auto.active && g.now >= s_auto.releaseAt) {
        input_release(s_auto.btn);
        s_auto.active = false;
    }
}
static void auto_tap(btn_id_t b) {
    if (s_auto.active) return;
    input_press(b);
    s_auto.btn = b; s_auto.releaseAt = g.now + 130.0f; s_auto.active = true;
}
static void auto_hold(btn_id_t b, float ms) {
    if (s_auto.active) return;
    input_press(b);
    s_auto.btn = b; s_auto.releaseAt = g.now + ms; s_auto.active = true;
}

void app_autopilot(float dt) {
    (void)dt;
    auto_release_check();
    if (s_auto_last != g.cur) { s_auto_last = g.cur; s_auto_t = 0; s_auto_step = 0; return; }
    s_auto_t += dt;
    if (s_auto.active) return;
    if (g.overlay_active) {
        if (s_auto_t > 1.4f) { auto_tap(BTN_OK); s_auto_t = 0; }
        return;
    }
    switch (g.cur) {
    case SC_IMU_CALIB:
        if (s_auto_t > 4.0f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_HOME:
        s_settings_tours = 0;
        s_hist_visited = false;
        if (s_auto_t > 2.0f) {
            if (g.demoCycle % 3 == 0) { auto_tap(BTN_OK); }
            else if (g.demoCycle % 3 == 1) { auto_tap(BTN_DOWN); }
            else { auto_tap(BTN_UP); }
            g.demoCycle++;
            s_auto_t = 0;
        }
        break;
    case SC_MODE_SELECT:
        if (s_auto_t > 1.6f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_NEW_TRACK_MARK_START:
        if (s_auto_t > 2.4f) { auto_tap(BTN_PUSH); s_auto_t = 0; }
        break;
    case SC_MOUNT_SELECT:
        if (s_auto_t > 2.0f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_RACING:
        if (s_auto_t > 5.0f) {
            if (s_auto_step < 3) { auto_tap(BTN_UP); s_auto_step++; }
            else { auto_hold(BTN_BACK, 900.0f); }
            s_auto_t = 0;
        }
        break;
    case SC_RACING_RECORDED:
        if (s_auto_t > 5.0f) { auto_tap(BTN_BACK); s_auto_t = 0; }
        break;
    case SC_LAP_SELECT:
        if (s_auto_t > 2.0f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_SECTION_MARKING:
        if (s_auto_t > 2.0f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_VEHICLE_SELECT:
        if (s_auto_t > 1.6f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_TRACK_NAMING:
        if (s_auto_t > 1.6f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_TRACK_LIST:
        if (s_auto_t > 1.6f) { auto_tap(BTN_OK); s_auto_t = 0; }
        break;
    case SC_HISTORY:
        if (s_hist_visited) { if (s_auto_t > 1.2f) { auto_tap(BTN_BACK); s_auto_t = 0; } break; }
        if (s_auto_step == 0 && s_auto_t > 1.4f) { auto_tap(BTN_OK); s_auto_step = 1; s_auto_t = 0; }
        else if (s_auto_step == 1 && s_auto_t > 2.4f) { auto_tap(BTN_OK); s_auto_step = 2; s_auto_t = 0; }
        break;
    case SC_HISTORY_DETAIL:
        if (s_auto_t > 3.0f) { s_hist_visited = true; auto_tap(BTN_BACK); s_auto_t = 0; }
        break;
    case SC_TRACK_EDIT:
        if (s_auto_step == 0 && s_auto_t > 1.4f) { auto_tap(BTN_OK); s_auto_step = 1; s_auto_t = 0; }
        else if (s_auto_step == 1 && s_auto_t > 1.6f) { auto_tap(BTN_OK); s_auto_step = 2; s_auto_t = 0; }
        else if (s_auto_step == 2 && s_auto_t > 1.2f) { auto_tap(BTN_BACK); s_auto_step = 3; s_auto_t = 0; }
        else if (s_auto_step == 3 && s_auto_t > 1.2f) { auto_tap(BTN_BACK); s_auto_step = 4; s_auto_t = 0; }
        break;
    case SC_SETTINGS:
        if (s_settings_tours > 0) {
            if (s_auto_t > 1.0f) { auto_tap(BTN_BACK); s_settings_tours = 0; s_auto_t = 0; }
            break;
        }
        /* note: sel starts on BRIGHTNESS (row 0); move down first so the demo
         * never touches the backlight while we are testing brightness = 0 */
        if (s_auto_step == 0 && s_auto_t > 1.4f) { auto_tap(BTN_OK); s_auto_step = 1; s_auto_t = 0; }
        else if (s_auto_step == 1 && s_auto_t > 0.8f) { auto_tap(BTN_DOWN); s_auto_step = 2; s_auto_t = 0; }  /* LED COLOR */
        else if (s_auto_step == 2 && s_auto_t > 0.8f) { auto_tap(BTN_DOWN); s_auto_step = 3; s_auto_t = 0; }  /* THEME */
        else if (s_auto_step == 3 && s_auto_t > 1.0f) { auto_tap(BTN_PUSH); s_auto_step = 4; s_auto_t = 0; }  /* toggle theme */
        else if (s_auto_step >= 4 && s_auto_step <= 7 && s_auto_t > 0.6f) { auto_tap(BTN_DOWN); s_auto_step++; s_auto_t = 0; } /* -> GNSS */
        else if (s_auto_step == 8 && s_auto_t > 1.0f) { s_settings_tours = 1; auto_tap(BTN_OK); s_auto_step = 9; s_auto_t = 0; }
        break;
    case SC_GPS_DEBUG:
        if (s_auto_step == 0 && s_auto_t > 2.0f) { auto_tap(BTN_DOWN); s_auto_step = 1; s_auto_t = 0; } /* sub-page */
        else if (s_auto_step == 1 && s_auto_t > 2.0f) { auto_tap(BTN_BACK); s_auto_step = 2; s_auto_t = 0; }
        break;
    default:
        break;
    }
}
#else
void app_autopilot(float dt) { (void)dt; }
#endif
