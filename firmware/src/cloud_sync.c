#include "cloud_sync.h"
#include "app.h"
#include "netcfg.h"
#include "tstore.h"
#include "log_store.h"
#include "race_store.h"
#include "geo.h"
#include "config.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "cloud";
#define CLOUD_BASE "https://racebrick.tfworks.cn"
#define CLOUD_BODY_START (384 * 1024)
#define CLOUD_BODY_MAX   (3 * 1024 * 1024)

static char s_uid[24] = "esp32s3-000000";
static char s_secret[33];
static int64_t s_last_time;
static uint32_t s_last_size;
static uint32_t s_upload_bytes;
static uint8_t *s_body;
static size_t s_body_len;
static int s_track_count;
static int s_session_count;
static int s_log_count;
static volatile bool s_ready;
static volatile bool s_prompted;
static volatile bool s_running;
static char s_result[64];
static volatile bool s_has_result;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static bool wifi_ok(void) { return netcfg_connected(); }

static void set_result(const char *msg) {
    taskENTER_CRITICAL(&s_mux);
    snprintf(s_result, sizeof(s_result), "%s", msg);
    s_has_result = true;
    taskEXIT_CRITICAL(&s_mux);
}

bool cloud_sync_take_result(char *buf, int cap) {
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_has_result) {
        if (buf && cap > 0) snprintf(buf, cap, "%s", s_result);
        s_has_result = false;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

const char *cloud_sync_uid(void) { return s_uid; }

bool cloud_sync_ready(void) {
    bool r;
    taskENTER_CRITICAL(&s_mux);
    r = s_ready && !s_prompted && !s_running;
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

bool cloud_sync_take_ready(void) {
    bool r = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_ready && !s_prompted && !s_running) {
        s_prompted = true;
        r = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return r;
}

static void fmt_size(char *out, int cap, uint32_t b) {
    if (b >= 1024u * 1024u) {
        uint32_t t = (uint32_t)(((uint64_t)b * 100u + (1u << 19)) >> 20);
        snprintf(out, cap, "%u.%02u MB", (unsigned)(t / 100), (unsigned)(t % 100));
    } else if (b >= 1024u) {
        snprintf(out, cap, "%u KB", (unsigned)((b + 512u) / 1024u));
    } else {
        snprintf(out, cap, "%u B", (unsigned)b);
    }
}

static void fmt_time(char *out, int cap, int64_t t) {
    if (t < 1700000000) {
        snprintf(out, cap, "--");
        return;
    }
    time_t tt = (time_t)t;
    struct tm tmv;
    if (localtime_r(&tt, &tmv) && strftime(out, cap, "%m-%d %H:%M", &tmv) > 0) return;
    snprintf(out, cap, "--");
}

void cloud_sync_summary(char *buf, int cap) {
    char when[24], last[16], up[16];
    fmt_time(when, sizeof(when), s_last_time);
    fmt_size(last, sizeof(last), s_last_size);
    fmt_size(up, sizeof(up), s_upload_bytes);
    snprintf(buf, cap, "SYNC TO CLOUD?\nUID %s\nLast %s  %s\nUpload %s  %dT/%dL",
             s_uid, when, last, up, s_track_count, s_log_count);
}

static void gen_secret(void) {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        s_secret[i * 2] = hex[r[i] >> 4];
        s_secret[i * 2 + 1] = hex[r[i] & 15];
    }
    s_secret[32] = 0;
}

static void identity_load(void) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        snprintf(s_uid, sizeof(s_uid), "esp32s3-%02X%02X%02X", mac[3], mac[4], mac[5]);

    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READWRITE, &h) != ESP_OK) {
        gen_secret();
        return;
    }
    size_t len = sizeof(s_secret);
    if (nvs_get_str(h, "secret", s_secret, &len) != ESP_OK || strlen(s_secret) < 16) {
        gen_secret();
        nvs_set_str(h, "secret", s_secret);
    }
    if (nvs_get_i64(h, "last_time", &s_last_time) != ESP_OK) s_last_time = 0;
    if (nvs_get_u32(h, "last_size", &s_last_size) != ESP_OK) s_last_size = 0;
    nvs_commit(h);
    nvs_close(h);
}

static void save_last_sync(uint32_t bytes) {
    time_t now = time(NULL);
    s_last_time = now;
    s_last_size = bytes;
    nvs_handle_t h;
    if (nvs_open("cloud", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "last_time", (int64_t)now);
    nvs_set_u32(h, "last_size", bytes);
    nvs_commit(h);
    nvs_close(h);
}

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    bool ok;
} obuf_t;

static bool ob_reserve(obuf_t *o, size_t add) {
    if (!o->ok) return false;
    if (o->len + add <= o->cap) return true;
    size_t cap = o->cap ? o->cap : CLOUD_BODY_START;
    while (cap < o->len + add) cap *= 2;
    if (cap > CLOUD_BODY_MAX) {
        o->ok = false;
        return false;
    }
    uint8_t *p = (uint8_t *)heap_caps_realloc(o->p, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        o->ok = false;
        return false;
    }
    o->p = p;
    o->cap = cap;
    return true;
}

static void ob_raw(obuf_t *o, const char *s, size_t n) {
    if (!ob_reserve(o, n)) return;
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void ob_puts(obuf_t *o, const char *s) { ob_raw(o, s, strlen(s)); }
static void ob_char(obuf_t *o, char c) { ob_raw(o, &c, 1); }

static void ob_printf(obuf_t *o, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(tmp)) {
        o->ok = false;
        return;
    }
    ob_raw(o, tmp, (size_t)n);
}

static void ob_json(obuf_t *o, const char *s) {
    ob_char(o, '"');
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
        case '"': ob_puts(o, "\\\""); break;
        case '\\': ob_puts(o, "\\\\"); break;
        case '\n': ob_puts(o, "\\n"); break;
        case '\r': ob_puts(o, "\\r"); break;
        case '\t': ob_puts(o, "\\t"); break;
        default:
            if (*p < 0x20) ob_printf(o, "\\u%04x", (unsigned)*p);
            else ob_char(o, (char)*p);
            break;
        }
    }
    ob_char(o, '"');
}

typedef struct {
    uint8_t b[3];
    int n;
} b64_t;

static void b64_feed(b64_t *c, obuf_t *o, const uint8_t *p, size_t n) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < n; i++) {
        c->b[c->n++] = p[i];
        if (c->n == 3) {
            char out[4] = {
                tab[c->b[0] >> 2],
                tab[((c->b[0] & 3) << 4) | (c->b[1] >> 4)],
                tab[((c->b[1] & 15) << 2) | (c->b[2] >> 6)],
                tab[c->b[2] & 63],
            };
            ob_raw(o, out, 4);
            c->n = 0;
        }
    }
}

static void b64_done(b64_t *c, obuf_t *o) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (c->n == 1) {
        char out[4] = {tab[c->b[0] >> 2], tab[(c->b[0] & 3) << 4], '=', '='};
        ob_raw(o, out, 4);
    } else if (c->n == 2) {
        char out[4] = {
            tab[c->b[0] >> 2],
            tab[((c->b[0] & 3) << 4) | (c->b[1] >> 4)],
            tab[(c->b[1] & 15) << 2],
            '=',
        };
        ob_raw(o, out, 4);
    }
    c->n = 0;
}

static float track_start_f(const track_t *t) {
    float sf = 0;
    if (!t || !t->geo || t->nscale <= 1e-9f) return 0;
    geo_ref_t r = {t->glat0, t->glon0};
    float mx, my, sdist, ddist;
    geo_meters(&r, t->line_lat, t->line_lon, &mx, &my);
    pt_t q = {mx * t->nscale + t->nox, my * t->nscale + t->noy};
    if (track_project(t, q, &sdist, &ddist)) sf = sdist / (float)t->N;
    return sf;
}

static bool add_track(obuf_t *o, const track_t *t, int slot, bool *first) {
    if (!*first) ob_char(o, ',');
    *first = false;
    float sf = track_start_f(t);
    ob_printf(o, "{\"local_id\":%d,\"payload\":{\"id\":%d,\"name\":", slot, slot);
    ob_json(o, t->name);
    ob_printf(o, ",\"geo\":%d,\"length_m\":%.1f,\"width\":%.1f,"
                 "\"start_width\":%.1f,\"start_f\":%.4f,\"reverse\":%d,"
                 "\"start_offset\":%.2f,\"start\":{\"lat\":%.7f,\"lon\":%.7f,"
                 "\"hdg\":%.3f},\"glat0\":%.7f,\"glon0\":%.7f,"
                 "\"nscale\":%.9f,\"nox\":%.6f,\"noy\":%.6f,\"sections\":[",
              t->geo ? 1 : 0, (double)t->lengthM, (double)t->widthM,
              (double)t->start_width, (double)sf, t->reverse ? 1 : 0,
              (double)t->start_offset, t->line_lat, t->line_lon,
              (double)t->line_hdg, t->glat0, t->glon0, (double)t->nscale,
              (double)t->nox, (double)t->noy);
    for (int i = 0; i < t->nsec && i < MAX_SECTIONS; i++)
        ob_printf(o, "%s%.5f", i ? "," : "", (double)t->sections[i]);
    ob_puts(o, "],\"points\":[");
    for (int i = 0; i < t->N && i < TRK_N; i++)
        ob_printf(o, "%s[%.4f,%.4f]", i ? "," : "", (double)t->points[i].x,
                  (double)t->points[i].y);
    ob_puts(o, "]}}");
    return o->ok;
}

static bool add_log(obuf_t *o, const histrec_t *r, int idx, bool *first) {
    if (!*first) ob_char(o, ',');
    *first = false;
    ob_printf(o, "{\"local_index\":%d,\"payload\":{\"index\":%d,\"track_name\":",
              idx, idx);
    ob_json(o, r->trackName);
    ob_puts(o, ",\"date\":");
    ob_json(o, r->date);
    ob_printf(o, ",\"mode\":%d,\"duration\":%.3f,\"laps\":%d,\"best\":%.3f,"
                 "\"best_index\":%d,\"has_best\":%d,\"vehicle\":%d,\"lap_times\":[",
              r->mode, (double)r->duration, r->lapCount, (double)r->bestTime,
              r->bestIndex, r->hasBest ? 1 : 0, r->vehicle);
    for (int i = 0; i < r->lapCount && i < HISTORY_LAPS; i++)
        ob_printf(o, "%s%.3f", i ? "," : "", (double)r->lapTime[i]);
    ob_puts(o, "],\"trace\":[");
    for (int i = 0; i < r->nTrace && i < TRACE_MAX; i++)
        ob_printf(o, "%s[%.5f,%.5f]", i ? "," : "", (double)r->trace[i].x,
                  (double)r->trace[i].y);
    ob_puts(o, "]}}");
    return o->ok;
}

static int64_t session_lap1_cutoff(int slot, const race_hdr_t *h) {
    if (h->lap_count < 1 || h->lap_time[0] <= 0.0f) return 0;
    int64_t last = -1;
    for (uint32_t i = 0; i < h->sample_count; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(slot, (int)i, &sm)) break;
        if (sm.lap == 1 && (sm.flags & 1)) last = sm.t_ms;
    }
    if (last < 0) return 0;
    int64_t cut = last - (int64_t)(h->lap_time[0] * 1000.0f);
    return cut > 0 ? cut : 0;
}

static bool add_session(obuf_t *o, int slot, const race_hdr_t *h, bool *first) {
    if (!*first) ob_char(o, ',');
    *first = false;
    ob_printf(o, "{\"local_slot\":%d,\"payload\":{\"slot\":%d,\"id\":%lu,"
                 "\"track\":%u,\"track_name\":",
              slot, slot, (unsigned long)h->id, h->track_slot);
    ob_json(o, h->track_name);
    ob_printf(o, ",\"start\":%lu,\"duration_ms\":%lu,\"samples\":%lu,"
                 "\"laps\":%u,\"deleted_laps\":%u,\"lap_times\":[",
              (unsigned long)h->start_epoch, (unsigned long)h->duration_ms,
              (unsigned long)h->sample_count, h->lap_count, h->lap_deleted_mask);
    for (int i = 0; i < h->lap_count && i < RACE_LAPS; i++)
        ob_printf(o, "%s%.3f", i ? "," : "", (double)h->lap_time[i]);
    ob_puts(o, "]},\"csv_b64\":\"");

    b64_t b64 = {0};
    char line[256];
    int n = snprintf(line, sizeof(line),
        "Format,3\nSession title,\"%s\"\nSession type,Lap timing\nTrack name,\"%s\"\n"
        "timestamp,fragment_id,lap_number,elapsed_time,latitude,longitude,speed\n"
        "unix time,,,s,deg,deg,m/s\n"
        ",,,,100: gps,100: gps,100: gps\n", h->track_name, h->track_name);
    b64_feed(&b64, o, (const uint8_t *)line, (size_t)n);

    int64_t l1cut = session_lap1_cutoff(slot, h);
    uint32_t per = h->lap_count ? ((uint32_t)h->lap_count * 96u) : 96u;
    uint32_t step = h->sample_count > per ? (h->sample_count + per - 1) / per : 1;
    if (step < 1) step = 1;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < h->sample_count; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(slot, (int)i, &sm)) break;
        if (sm.lap == 0 || sm.lap > h->lap_count) continue;
        if (!(sm.flags & 1)) continue;
        if (sm.lap == 1 && (int64_t)sm.t_ms < l1cut) continue;
        if (h->lap_deleted_mask & (1u << (sm.lap - 1))) continue;
        if ((seen++ % step) != 0) continue;
        n = snprintf(line, sizeof(line), "%.3f,0,%u,%.3f,%.7f,%.7f,%.3f\n",
                     (double)h->start_epoch + sm.t_ms / 1000.0, sm.lap,
                     sm.t_ms / 1000.0, sm.lat_e7 / 1e7, sm.lon_e7 / 1e7,
                     sm.speed10 / 36.0);
        b64_feed(&b64, o, (const uint8_t *)line, (size_t)n);
    }
    b64_done(&b64, o);
    ob_puts(o, "\"}");
    return o->ok;
}

static bool build_payload(void) {
    obuf_t o = {0};
    o.ok = true;
    ob_printf(&o, "{\"device_uid\":\"%s\",\"device_secret\":\"%s\",\"since\":0,"
                  "\"tracks\":[", s_uid, s_secret);
    int tracks = 0, sessions = 0, logs = 0;
    bool first = true;

    track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t) {
        int slots[TSTORE_EXT_SLOTS];
        int ns = tstore_slots(slots, TSTORE_EXT_SLOTS);
        for (int i = 0; i < ns; i++) {
            if (!tstore_load(slots[i], t)) continue;
            if (!add_track(&o, t, slots[i], &first)) break;
            tracks++;
        }
        free(t);
    }
    if (!o.ok) goto fail;

    ob_puts(&o, "],\"logs\":[");
    first = true;
    histrec_t *arr = (histrec_t *)heap_caps_malloc(
        sizeof(histrec_t) * HISTORY_RECORDS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (arr) {
        int nrec = log_store_load(arr, HISTORY_RECORDS);
        for (int i = 0; i < nrec; i++) {
            if (!add_log(&o, &arr[i], i, &first)) break;
            logs++;
        }
        free(arr);
    }
    if (!o.ok) goto fail;

    ob_puts(&o, "],\"sessions\":[");
    first = true;
    for (int i = 0; i < RACE_SLOTS; i++) {
        race_hdr_t h;
        if (!race_store_info(i, &h)) continue;
        if (!add_session(&o, i, &h, &first)) break;
        sessions++;
    }
    if (!o.ok) goto fail;
    ob_puts(&o, "],\"command_acks\":[]}");

    s_upload_bytes = (uint32_t)o.len;
    s_body = o.p;
    s_body_len = o.len;
    s_track_count = tracks;
    s_session_count = sessions;
    s_log_count = logs;
    ESP_LOGI(TAG, "prepared %u bytes: %d tracks, %d sessions, %d logs",
             (unsigned)o.len, tracks, sessions, logs);
    return true;

fail:
    ESP_LOGW(TAG, "payload build failed at %u bytes", (unsigned)o.len);
    free(o.p);
    return false;
}

typedef struct {
    char *buf;
    int cap;
    int len;
} rxbuf_t;

static esp_err_t http_event(esp_http_client_event_t *ev) {
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->user_data && ev->data_len > 0) {
        rxbuf_t *rx = (rxbuf_t *)ev->user_data;
        if (rx->buf && rx->len + ev->data_len < rx->cap) {
            memcpy(rx->buf + rx->len, ev->data, (size_t)ev->data_len);
            rx->len += ev->data_len;
            rx->buf[rx->len] = 0;
        }
    }
    return ESP_OK;
}

static int http_req(const char *path, bool post, const uint8_t *body, int blen,
                    char *rx, int rxcap) {
    char url[192];
    snprintf(url, sizeof(url), "%s%s", CLOUD_BASE, path);
    rxbuf_t rxb = {rx, rxcap, 0};
    if (rx && rxcap > 0) rx[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 30000,
        .disable_auto_redirect = true,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_data = &rxb,
        .event_handler = http_event,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    if (s_secret[0]) esp_http_client_set_header(c, "X-Device-Secret", s_secret);
    if (post) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_post_field(c, (const char *)(body ? body : (const uint8_t *)""), blen);
    }
    esp_err_t err = esp_http_client_perform(c);
    int code = err == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    if (err != ESP_OK) ESP_LOGW(TAG, "%s failed: %s", path, esp_err_to_name(err));
    else ESP_LOGI(TAG, "%s -> %d", path, code);
    esp_http_client_cleanup(c);
    return code;
}

static bool register_device(void) {
    char body[320];
    int n = snprintf(body, sizeof(body),
        "{\"device_uid\":\"%s\",\"device_secret\":\"%s\","
        "\"board\":\"ESP32-S3\",\"fw\":\"cloud-1\"}", s_uid, s_secret);
    return http_req("/api/device/v1/register", true, (const uint8_t *)body, n, NULL, 0) == 200;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool name_decode(const char *in, char *out, int cap) {
    int val = 0, bits = -8, n = 0;
    for (const char *p = in; p && *p && *p != '='; p++) {
        int v = b64_value(*p);
        if (v < 0) return false;
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            if (n < cap - 1) out[n++] = (char)((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    out[n] = 0;
    return n > 0;
}

static void apply_track_command(int cid, int local, float sf, float width,
                                float offset, int reverse, const char *name_b64) {
    track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) return;
    if (!tstore_load(local, t)) {
        free(t);
        char path[160];
        snprintf(path, sizeof(path), "/api/device/v1/ack?uid=%s&id=%d", s_uid, cid);
        http_req(path, true, NULL, 0, NULL, 0);
        return;
    }
    char old_name[48];
    snprintf(old_name, sizeof(old_name), "%s", t->name);
    char new_name[49];
    bool has_name = name_b64 && name_b64[0] && name_decode(name_b64, new_name, sizeof(new_name));
    if (has_name) snprintf(t->name, sizeof(t->name), "%s", new_name);
    if (width < 1.0f) width = 1.0f;
    if (width > 50.0f) width = 50.0f;
    if (offset < -50.0f) offset = -50.0f;
    if (offset > 50.0f) offset = 50.0f;
    sf = fmodf(sf, 1.0f);
    if (sf < 0) sf += 1.0f;
    t->start_width = width;
    t->start_offset = offset;
    t->reverse = reverse ? 1 : 0;
    if (t->geo && t->nscale > 1e-9f) {
        pt_t q = track_point_at(t, sf);
        pt_t a = track_point_at(t, sf - 0.004f);
        pt_t b = track_point_at(t, sf + 0.004f);
        float hdg = atan2f(b.y - a.y, b.x - a.x);
        if (t->reverse) hdg += (float)M_PI;
        float offn = t->start_offset * t->nscale;
        q.x += sinf(hdg) * offn;
        q.y += -cosf(hdg) * offn;
        geo_ref_t r = {t->glat0, t->glon0};
        geo_from_meters(&r, (q.x - t->nox) / t->nscale,
                        (q.y - t->noy) / t->nscale,
                        &t->line_lat, &t->line_lon);
        t->line_hdg = hdg;
    }
    bool ok = tstore_save(local, t);
    if (ok) {
        if (has_name && strcmp(old_name, t->name) != 0) {
            log_store_rename(old_name, t->name);
            race_store_rename(old_name, t->name);
            if (g.log) {
                for (int i = 0; i < g.nlog; i++)
                    if (strcmp(g.log[i].trackName, old_name) == 0)
                        snprintf(g.log[i].trackName, sizeof(g.log[i].trackName), "%s", t->name);
            }
        }
        for (int i = 0; i < g.ntracks; i++)
            if (g.tracks[i].store_slot == local) g.tracks[i] = *t;
        if (g.engine_valid && g.engine.track && g.engine.track->store_slot == local)
            *g.engine.track = *t;
        char path[160];
        snprintf(path, sizeof(path), "/api/device/v1/ack?uid=%s&id=%d", s_uid, cid);
        http_req(path, true, NULL, 0, NULL, 0);
        ESP_LOGI(TAG, "applied track command %d to slot %d", cid, local);
    }
    free(t);
}

static void pull_commands(void) {
    char path[192], rx[4096];
    snprintf(path, sizeof(path), "/api/device/v1/commands?uid=%s", s_uid);
    if (http_req(path, false, NULL, 0, rx, sizeof(rx)) != 200) return;
    char *line = rx;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        int cid, local, reverse;
        float sf, width, offset;
        char name_b64[72] = {0};
        int got = sscanf(line, "%d|%d|%f|%f|%f|%d|%63s", &cid, &local, &sf,
                         &width, &offset, &reverse, name_b64);
        if (got >= 6) {
            apply_track_command(cid, local, sf, width, offset, reverse, name_b64);
        }
        line = nl ? nl + 1 : NULL;
    }
}

static void sync_task(void *arg) {
    (void)arg;
    int code = http_req("/api/device/v1/sync?nodata=1", true, s_body,
                        (int)s_body_len, NULL, 0);
    char msg[48];
    uint32_t bytes = s_upload_bytes;
    if (code == 200) {
        save_last_sync(bytes);
        snprintf(msg, sizeof(msg), "CLOUD SYNC OK");
        pull_commands();
    } else {
        snprintf(msg, sizeof(msg), "CLOUD SYNC FAILED %d", code);
    }
    free(s_body);
    s_body = NULL;
    s_body_len = 0;
    s_upload_bytes = 0;
    s_running = false;
    set_result(msg);
    vTaskDelete(NULL);
}

void cloud_sync_start(void) {
    taskENTER_CRITICAL(&s_mux);
    if (!s_ready || s_running) {
        taskEXIT_CRITICAL(&s_mux);
        return;
    }
    s_running = true;
    s_ready = false;
    taskEXIT_CRITICAL(&s_mux);
    if (xTaskCreate(sync_task, "cloud_sync", 12288, NULL, 4, NULL) != pdPASS) {
        s_running = false;
        set_result("CLOUD TASK FAILED");
    }
}

void cloud_sync_cancel(void) {
    taskENTER_CRITICAL(&s_mux);
    s_ready = false;
    s_prompted = true;
    taskEXIT_CRITICAL(&s_mux);
    free(s_body);
    s_body = NULL;
    s_body_len = 0;
    s_upload_bytes = 0;
}

static void clear_snapshot(void) {
    free(s_body);
    s_body = NULL;
    s_body_len = 0;
    s_upload_bytes = 0;
    s_ready = false;
}

static void cloud_task(void *arg) {
    (void)arg;
    bool connected = false;
    bool health = false;
    for (;;) {
        if (!wifi_ok()) {
            if (connected) {
                connected = false;
                health = false;
                if (!s_running) {
                    s_prompted = false;
                    clear_snapshot();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (!connected) {
            connected = true;
            health = false;
            if (!s_running) {
                s_prompted = false;
                clear_snapshot();
            }
            ESP_LOGI(TAG, "wifi connected, probing cloud");
        }
        if (!health) {
            int hc = http_req("/healthz", false, NULL, 0, NULL, 0);
            if (hc == 200) {
                health = true;
                if (!register_device()) {
                    health = false;
                    ESP_LOGW(TAG, "device register failed");
                    vTaskDelay(pdMS_TO_TICKS(15000));
                    continue;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(15000));
                continue;
            }
        }
        if (health && !s_ready && !s_prompted && !s_running) {
            if (build_payload()) {
                s_ready = true;
            } else {
                s_prompted = true;
                set_result("CLOUD PREP FAILED");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void cloud_sync_init(void) {
    identity_load();
    ESP_LOGI(TAG, "uid=%s last=%u bytes", s_uid, (unsigned)s_last_size);
    if (xTaskCreate(cloud_task, "cloud", 12288, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "task create failed");
}
