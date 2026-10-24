#include "nav.h"
#include "config.h"
#include "gps.h"
#include "netcfg.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "nav";

#define NAV_URL        "http://47.100.182.210:2020/api/v1/nav"
#define NAV_SIZE_M     400        /* ground side length (m) */
#define NAV_BODY_MAX   (48 * 1024)

static uint16_t *s_front = NULL, *s_back = NULL;
static uint8_t  *s_body = NULL;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;

static volatile bool s_active = false;
static volatile bool s_force = false;
static volatile bool s_have = false;
static volatile int  s_status = 0;
static volatile int  s_http_code = 0;
static int           s_fail = 0;
static volatile int  s_frames = 0;
static volatile int  s_bytes = 0;
static volatile int  s_w = 0, s_h = 0;
static volatile int64_t s_last_us = 0;

static uint32_t s_seq = 0;
static char s_devid[24] = "esp32s3-1";
static double s_last_lat = 0, s_last_lon = 0;
static float  s_last_hdg = 0;
static int    s_mode = 0;     /* 0=road 1=satellite 2=blend */
static int    s_sat = 60;     /* blend satellite opacity */
static bool   s_req = false;
static int64_t s_req_us = 0;

int nav_status(void) { return s_status; }
int nav_http_code(void) { return s_http_code; }
int nav_frames(void) { return s_frames; }
int nav_last_bytes(void) { return s_bytes; }
int nav_age_ms(void) { return s_last_us ? (int)((esp_timer_get_time() - s_last_us) / 1000) : -1; }

const uint16_t *nav_frame_lock(int *w, int *h) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (w) *w = s_w;
    if (h) *h = s_h;
    return s_have ? s_front : NULL;
}
void nav_frame_unlock(void) { xSemaphoreGive(s_lock); }

static float dist_m(double la1, double lo1, double la2, double lo2) {
    double dlat = (la2 - la1) * 111320.0;
    double dlon = (lo2 - lo1) * 111320.0 * cos((la1 + la2) * 0.5 * M_PI / 180.0);
    return (float)sqrt(dlat * dlat + dlon * dlon);
}

static float wrap180(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* Stable heading for the heading-up map.
 * GPS course-over-ground is only meaningful while moving; when slow it is
 * random noise, which made the map/arrow spin. Hold the last heading below
 * ~1.5 km/h and low-pass the rest (like a nav app's compass smoothing). */
static float s_hdg = 0;
static bool  s_hdg_ok = false;
#define HDG_MIN_KMH 1.5f

static float hdg_update(float raw, float kmh, float dt) {
    /* never moved yet: COG is pure noise, hold north-up instead of spinning */
    if (kmh < HDG_MIN_KMH) return s_hdg_ok ? s_hdg : 0.0f;
    if (!s_hdg_ok) { s_hdg = raw; s_hdg_ok = true; return s_hdg; }
    if (dt > 0.5f) dt = 0.5f;
    float k = 1.0f - expf(-dt / 0.35f);      /* ~0.35 s time constant */
    s_hdg = wrap180(s_hdg + wrap180(raw - s_hdg) * k);
    return s_hdg;
}

float nav_heading(void) { return s_hdg_ok ? s_hdg : 0.0f; }

/* ---- HTTP POST, read the pal16 body ---- */
static int http_fetch(const char *json, int jlen) {
    s_http_code = 0;
    esp_http_client_config_t cfg = {
        .url = NAV_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "Content-Type", "application/json");

    int ret = -1;
    if (esp_http_client_open(c, jlen) == ESP_OK) {
        int wr = esp_http_client_write(c, json, jlen);
        if (wr == jlen) {
            int clen = esp_http_client_fetch_headers(c);
            int code = esp_http_client_get_status_code(c);
            s_http_code = code;
            if (code == 200) {
                int total = 0;
                if (clen > 0) {
                    while (total < clen && total < NAV_BODY_MAX) {
                        int want = clen - total;
                        if (want > NAV_BODY_MAX - total) want = NAV_BODY_MAX - total;
                        int r = esp_http_client_read(c, (char *)s_body + total, want);
                        if (r <= 0) break;
                        total += r;
                    }
                    if (total == clen) ret = total;
                    else ESP_LOGW(TAG, "short body %d/%d", total, clen);
                } else {   /* chunked fallback */
                    for (;;) {
                        int r = esp_http_client_read(c, (char *)s_body + total, NAV_BODY_MAX - total);
                        if (r <= 0) break;
                        total += r;
                        if (total >= NAV_BODY_MAX) break;
                    }
                    if (total > 0) ret = total;
                }
            } else {
                ESP_LOGW(TAG, "HTTP status %d", code);
            }
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ret;
}

/* ---- pal16 decoder (little-endian header + RGB565 palette + RLE) ---- */
static bool pal16_decode(const uint8_t *b, int len, uint16_t *out, int *ow, int *oh) {
    if (len < 6) return false;
    int w = b[0] | (b[1] << 8);
    int h = b[2] | (b[3] << 8);
    int pc = b[4] | (b[5] << 8);
    if (w <= 0 || h <= 0 || w > NAV_IMG_SIZE || h > NAV_IMG_SIZE || pc <= 0 || pc > 16) return false;
    const uint8_t *p = b + 6, *end = b + len;
    if (p + pc * 2 > end) return false;
    uint16_t pal[16];
    for (int i = 0; i < pc; i++) { pal[i] = (uint16_t)(p[0] | (p[1] << 8)); p += 2; }

    int total = w * h, idx = 0;
    while (idx < total && p < end) {
        uint8_t v = *p++;
        int run = (v >> 4) + 1;
        int col = v & 0x0F;
        if ((v >> 4) == 15) { if (p >= end) break; run = 16 + (*p++); }
        uint16_t c = (col < pc) ? pal[col] : 0;
        for (int k = 0; k < run && idx < total; k++) out[idx++] = c;
    }
    if (idx != total) return false;
    *ow = w; *oh = h;
    return true;
}

static void nav_task(void *arg) {
    (void)arg;
    int64_t tprev = 0;
    for (;;) {
        if (!s_active) { tprev = 0; vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        gps_state_t *gp = gps_get();
        /* keep the filtered heading warm even when no fetch is due */
        int64_t now = esp_timer_get_time();
        float dt = tprev ? (float)(now - tprev) * 1e-6f : 0.1f;
        tprev = now;
        float hd = hdg_update(gp->course, gp->speed_kmh, dt);
        if (!netcfg_connected()) { s_status = 4; tprev = 0; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (!(gp->fix_type >= 1 && gp->has_pos)) { s_status = 5; tprev = 0; vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        /* rate-limit: satellite/blend tiles are ~27 KB and server-rendered, so
         * asking for a new seq on every 12 m / 12 deg change overloads it */
        int64_t wait_us = (int64_t)(s_mode == 0 ? 1200 : 2200) * 1000;
        if (s_fail) {   /* back off while the backend is erroring */
            int sh = s_fail > 3 ? 3 : s_fail - 1;
            int64_t b = ((int64_t)5 * 1000000) << sh;   /* 5/10/20/40 s */
            if (b > 30000000) b = 30000000;
            if (b > wait_us) wait_us = b;
        }
        if (s_req && !s_force && now - s_req_us < wait_us) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        double lat = gp->lat, lon = gp->lon;
        bool need = !s_req || s_force;
        if (s_req && !need) {
            if (dist_m(s_last_lat, s_last_lon, lat, lon) > 12.0f) need = true;
            if (fabsf(wrap180(hd - s_last_hdg)) > 12.0f) need = true;
            if (now - s_req_us > 2500000) need = true;
        }
        if (!need) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        s_force = false;

        s_seq++;
        char modejson[72];
        if (s_mode == 1) snprintf(modejson, sizeof modejson, "\"mode\":\"satellite\"");
        else if (s_mode == 2) snprintf(modejson, sizeof modejson,
                                       "\"mode\":\"blend\",\"sat_opacity\":%d", s_sat);
        else snprintf(modejson, sizeof modejson, "\"mode\":\"road\",\"theme\":\"gameboy\"");
        char json[256];
        int jl = snprintf(json, sizeof json,
            "{\"device_id\":\"%s\",\"seq\":%lu,\"lng\":%.7f,\"lat\":%.7f,"
            "\"size_m\":%d,\"heading\":%.1f,\"size\":%d,\"format\":\"pal16\","
            "%s,\"coord\":\"wgs84\"}",
            s_devid, (unsigned long)s_seq, lon, lat, NAV_SIZE_M, hd, NAV_IMG_SIZE, modejson);

        int bodyLen = -1;
        for (int attempt = 0; attempt < 3 && bodyLen < 0; attempt++) {
            if (attempt) vTaskDelay(pdMS_TO_TICKS(1000 << (attempt - 1)));
            bodyLen = http_fetch(json, jl);
            /* don't hammer a rate-limited / failing backend */
            if (bodyLen < 0 && (s_http_code == 429 || s_http_code == 502 || s_http_code == 503)) break;
        }
        s_req_us = esp_timer_get_time();
        s_last_lat = lat; s_last_lon = lon; s_last_hdg = hd; s_req = true;
        s_bytes = bodyLen;

        if (bodyLen > 0) {
            int w = 0, h = 0;
            if (pal16_decode(s_body, bodyLen, s_back, &w, &h)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                uint16_t *t = s_front; s_front = s_back; s_back = t;
                s_w = w; s_h = h; s_have = true;
                xSemaphoreGive(s_lock);
                s_status = 1;
                s_frames++;
                s_fail = 0;
                s_last_us = esp_timer_get_time();
                ESP_LOGI(TAG, "frame %d: %dx%d, %d bytes, mode=%d/%d, heap %u",
                         s_frames, w, h, bodyLen, s_mode, s_sat,
                         (unsigned)esp_get_free_heap_size());
            } else {
                s_status = 3;
                s_fail++;
                ESP_LOGW(TAG, "decode failed (%d bytes)", bodyLen);
            }
        } else {
            s_status = 2;
            s_fail++;
            ESP_LOGW(TAG, "fetch failed (HTTP %d, heap %u)", s_http_code,
                     (unsigned)esp_get_free_heap_size());
        }
        vTaskDelay(pdMS_TO_TICKS(80));
    }
}

void nav_start(void) {
    if (s_task) return;
    /* unique id + non-zero seq: the server caches by (device_id, seq) for 600 s,
     * so a seq restarting at 0 after reboot would serve stale tiles */
    s_seq = esp_random();
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(s_devid, sizeof s_devid, "esp32s3-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "device %s  seq base %lu", s_devid, (unsigned long)s_seq);
    s_front = (uint16_t *)heap_caps_malloc((size_t)NAV_IMG_SIZE * NAV_IMG_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_back  = (uint16_t *)heap_caps_malloc((size_t)NAV_IMG_SIZE * NAV_IMG_SIZE * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_body  = (uint8_t *)heap_caps_malloc(NAV_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock  = xSemaphoreCreateMutex();
    if (!s_front || !s_back || !s_body || !s_lock) { ESP_LOGE(TAG, "alloc failed"); return; }
    if (xTaskCreate(nav_task, "nav", 8192, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed"); s_task = NULL;
    }
}

void nav_set_active(bool on) {
    s_active = on;
    if (on) s_force = true;
}

void nav_set_mode(int mode, int sat_opacity) {
    if (mode < 0 || mode > 2) mode = 0;
    if (sat_opacity < 0) sat_opacity = 0;
    else if (sat_opacity > 100) sat_opacity = 100;
    if (mode != s_mode || sat_opacity != s_sat) s_force = true;
    s_mode = mode;
    s_sat = sat_opacity;
    ESP_LOGI(TAG, "mode -> %s%s", mode == 0 ? "road" : mode == 1 ? "satellite" : "blend",
             mode == 2 ? "" : "");
}

void nav_refresh(void) { s_force = true; }
