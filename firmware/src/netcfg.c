#include "netcfg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "gps.h"
#include "nav.h"
#include "cloud_sync.h"
#include "tstore.h"
#include "race_store.h"
#include "log_store.h"
#include "track_img.h"
#include "gfx.h"
#include "geo.h"
#include <math.h>
#include <time.h>
#include "psa/crypto.h"

static const char *TAG = "netcfg";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_cmd = 0;
static int s_goto = -1;
static int s_goto_enter = 0;
static int s_key = 0;
static volatile bool s_transfer_busy = false;
static volatile int64_t s_transfer_until_us = 0;

void netcfg_transfer_begin(void) {
    s_transfer_until_us = esp_timer_get_time() + 120000000LL;
    s_transfer_busy = true;
}

void netcfg_transfer_end(void) {
    s_transfer_busy = false;
    s_transfer_until_us = 0;
}

bool netcfg_transfer_active(void) {
    if (!s_transfer_busy) return false;
    if (esp_timer_get_time() > s_transfer_until_us) {
        s_transfer_busy = false;
        s_transfer_until_us = 0;
        return false;
    }
    return true;
}

int netcfg_take_cmd(void) {
    int c;
    taskENTER_CRITICAL(&s_mux);
    c = s_cmd; s_cmd = 0;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

int netcfg_take_goto(int *enter) {
    int n;
    taskENTER_CRITICAL(&s_mux);
    n = s_goto; s_goto = -1; *enter = s_goto_enter; s_goto_enter = 0;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

int netcfg_take_key(void) {
    int k;
    taskENTER_CRITICAL(&s_mux);
    k = s_key; s_key = 0;
    taskEXIT_CRITICAL(&s_mux);
    return k;
}

static httpd_handle_t s_httpd = NULL;
static bool s_started = false;
static bool s_connecting = false;
static bool s_connected = false;
static bool s_connect_failed = false;
static bool s_scanning = false;
static int64_t s_connect_t0 = 0;
static char s_ip[20] = "";
static char s_ap_ssid[24] = "RaceBrick";
static char s_sta_ssid[33] = "";

static void start_sta(const char *ssid, const char *pass);

#define SCAN_MAX 16
static wifi_ap_record_t s_aps[SCAN_MAX];
static int s_naps = 0;
static int64_t s_scan_us = 0;
#define SCAN_FRESH_US 3000000   /* reuse a scan up to 3 s old */

bool netcfg_running(void) { return s_started; }
bool netcfg_connected(void) { return s_connected; }

void netcfg_set_low_latency(bool on) {
    if (!s_started) return;
    esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_ps(%d) failed: %s", on, esp_err_to_name(err));
}
const char *netcfg_ip(void) { return s_ip; }
const char *netcfg_ap_ssid(void) { return s_ap_ssid; }

void netcfg_status(char *buf, int cap) {
    if (s_connected) {
        if (s_sta_ssid[0]) snprintf(buf, cap, "CONNECTED %s %s", s_sta_ssid, s_ip);
        else snprintf(buf, cap, "CONNECTED %s", s_ip);
    } else if (s_scanning) {
        snprintf(buf, cap, "SCANNING...");
    } else if (s_connecting) {
        int sec = (int)((esp_timer_get_time() - s_connect_t0) / 1000000);
        if (sec > 20) snprintf(buf, cap, "FAILED - CHECK SSID/PASSWORD");
        else snprintf(buf, cap, "CONNECTING... %ds", sec);
    } else if (s_connect_failed) {
        snprintf(buf, cap, "FAILED - CHECK SSID/PASSWORD");
    } else if (s_started) {
        snprintf(buf, cap, "AP: %s", s_ap_ssid);
    } else {
        snprintf(buf, cap, "NOT STARTED");
    }
}

/* ---------------- credentials (up to 8 remembered networks) ---------------- */
static void nvs_key(char *out, int cap, const char *prefix, int i) {
    snprintf(out, cap, "%s%d", prefix, i);
}

static int saved_read(char ssid[][33], char pass[][65]) {
    nvs_handle_t h;
    int n = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        int32_t cnt = 0;
        if (nvs_get_i32(h, "count", &cnt) == ESP_OK && cnt > 0) {
            if (cnt > NETCFG_SAVED_MAX) cnt = NETCFG_SAVED_MAX;
            for (int i = 0; i < cnt; i++) {
                char k[12];
                size_t l = 33;
                nvs_key(k, sizeof k, "s", i);
                if (nvs_get_str(h, k, ssid[n], &l) != ESP_OK || !ssid[n][0]) continue;
                l = 65;
                nvs_key(k, sizeof k, "p", i);
                if (nvs_get_str(h, k, pass[n], &l) != ESP_OK) pass[n][0] = 0;
                n++;
            }
        }
        nvs_close(h);
    }
    /* migrate the old single-network keys */
    if (n == 0) {
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            size_t l = 33;
            if (nvs_get_str(h, "ssid", ssid[0], &l) == ESP_OK && ssid[0][0]) {
                l = 65;
                if (nvs_get_str(h, "pass", pass[0], &l) != ESP_OK) pass[0][0] = 0;
                n = 1;
            }
            nvs_close(h);
        }
    }
    return n;
}

static void saved_write(char ssid[][33], char pass[][65], int n) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    if (n > NETCFG_SAVED_MAX) n = NETCFG_SAVED_MAX;
    nvs_set_i32(h, "count", n);
    for (int i = 0; i < n; i++) {
        char k[12];
        nvs_key(k, sizeof k, "s", i);
        nvs_set_str(h, k, ssid[i]);
        nvs_key(k, sizeof k, "p", i);
        nvs_set_str(h, k, pass[i]);
    }
    if (n > 0) {
        nvs_set_str(h, "ssid", ssid[n - 1]);
        nvs_set_str(h, "pass", pass[n - 1]);
        nvs_set_i32(h, "last", n - 1);
    } else {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_erase_key(h, "last");
    }
    nvs_commit(h);
    nvs_close(h);
}

static void save_creds(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    int idx = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s[i], ssid)) { idx = i; break; }
    if (idx < 0) {
        if (n >= NETCFG_SAVED_MAX) {
            for (int i = 1; i < NETCFG_SAVED_MAX; i++) {
                memcpy(s[i - 1], s[i], sizeof s[i]);
                memcpy(p[i - 1], p[i], sizeof p[i]);
            }
            n = NETCFG_SAVED_MAX - 1;
        }
        idx = n++;
        snprintf(s[idx], sizeof s[idx], "%s", ssid);
    }
    snprintf(p[idx], sizeof p[idx], "%s", pass ? pass : "");
    /* keep the last-used network at the end; move it there */
    if (idx != n - 1) {
        char ts[33], tp[65];
        memcpy(ts, s[idx], sizeof ts); memcpy(tp, p[idx], sizeof tp);
        for (int i = idx; i < n - 1; i++) {
            memcpy(s[i], s[i + 1], sizeof s[i]);
            memcpy(p[i], p[i + 1], sizeof p[i]);
        }
        memcpy(s[n - 1], ts, sizeof s[n - 1]);
        memcpy(p[n - 1], tp, sizeof p[n - 1]);
    }
    saved_write(s, p, n);
}

static bool load_creds(char *ssid, int sc, char *pass, int pc) {
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    if (n <= 0) return false;
    int idx = n - 1;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        int32_t last = -1;
        if (nvs_get_i32(h, "last", &last) == ESP_OK && last >= 0 && last < n) idx = last;
        nvs_close(h);
    }
    snprintf(ssid, sc, "%s", s[idx]);
    snprintf(pass, pc, "%s", p[idx]);
    return ssid[0] != 0;
}

bool netcfg_has_creds(void) {
    char ssid[33], pass[65];
    return load_creds(ssid, sizeof ssid, pass, sizeof pass);
}

int netcfg_wifi_saved_count(void) {
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    return saved_read(s, p);
}

bool netcfg_wifi_saved_get(int idx, char *ssid, int cap) {
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    if (idx < 0 || idx >= n || !ssid || cap <= 0) return false;
    snprintf(ssid, cap, "%s", s[idx]);
    return true;
}

bool netcfg_wifi_saved_has(const char *ssid) {
    if (!ssid || !ssid[0]) return false;
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    for (int i = 0; i < n; i++) if (!strcmp(s[i], ssid)) return true;
    return false;
}

bool netcfg_wifi_connect_saved(const char *ssid) {
    if (!ssid || !ssid[0] || !s_started) return false;
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    int idx = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s[i], ssid)) { idx = i; break; }
    if (idx < 0) return false;
    if (idx != n - 1) {
        char ts[33], tp[65];
        memcpy(ts, s[idx], sizeof ts); memcpy(tp, p[idx], sizeof tp);
        for (int i = idx; i < n - 1; i++) {
            memcpy(s[i], s[i + 1], sizeof s[i]);
            memcpy(p[i], p[i + 1], sizeof p[i]);
        }
        memcpy(s[n - 1], ts, sizeof s[n - 1]);
        memcpy(p[n - 1], tp, sizeof p[n - 1]);
        saved_write(s, p, n);
    }
    start_sta(s[n - 1], p[n - 1]);
    return true;
}

bool netcfg_wifi_forget(const char *ssid) {
    if (!ssid || !ssid[0]) return false;
    char s[NETCFG_SAVED_MAX][33], p[NETCFG_SAVED_MAX][65];
    int n = saved_read(s, p);
    int idx = -1;
    for (int i = 0; i < n; i++) if (!strcmp(s[i], ssid)) { idx = i; break; }
    if (idx < 0) return false;
    for (int i = idx; i < n - 1; i++) {
        memcpy(s[i], s[i + 1], sizeof s[i]);
        memcpy(p[i], p[i + 1], sizeof p[i]);
    }
    saved_write(s, p, n - 1);
    return true;
}

static void setstr(uint8_t *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void start_sta(const char *ssid, const char *pass) {
    snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", ssid ? ssid : "");
    s_connect_failed = false;
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    setstr(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    setstr(sta.sta.password, sizeof(sta.sta.password), pass);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* allow open/WPA/WPA2/WPA3 */
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_connected = false;
    s_connecting = true;
    s_connect_t0 = esp_timer_get_time();
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to '%s'...", ssid);
}

/* ---------------- events ---------------- */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_connecting) esp_wifi_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            if (s_connecting) {
                int sec = (int)((esp_timer_get_time() - s_connect_t0) / 1000000);
                if (sec < 20) esp_wifi_connect();
                else {
                    s_connecting = false;
                    s_connect_failed = true;
                    ESP_LOGW(TAG, "STA connect timed out after %ds", sec);
                }
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "phone joined AP");
        } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
            ESP_LOGI(TAG, "phone left AP");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        s_connect_failed = false;
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

/* ---------------- scan ---------------- */
static void do_scan(void) {
    wifi_scan_config_t sc;
    memset(&sc, 0, sizeof(sc));
    s_naps = 0;
    if (!s_started) return;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) { ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err)); return; }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > SCAN_MAX) num = SCAN_MAX;
    uint16_t got = num;
    if (got) esp_wifi_scan_get_ap_records(&got, s_aps);
    s_naps = got;
    s_scan_us = esp_timer_get_time();
    ESP_LOGI(TAG, "scan: %d APs", s_naps);
}

int netcfg_wifi_scan(void) {
    /* Scanning must not inherit a stale FAILED auto-connect status. Abort a
     * pending STA attempt, but leave an established connection alone. */
    s_connect_failed = false;
    if (s_connecting) {
        s_connecting = false;
        esp_wifi_disconnect();
    }
    s_scanning = true;
    do_scan();
    s_scanning = false;
    return s_naps;
}
int netcfg_wifi_scan_count(void) { return s_naps; }
bool netcfg_wifi_scan_get(int idx, char *ssid, int cap, int *rssi, int *auth) {
    if (idx < 0 || idx >= s_naps || !ssid || cap <= 0) return false;
    snprintf(ssid, cap, "%s", (const char *)s_aps[idx].ssid);
    if (rssi) *rssi = s_aps[idx].rssi;
    if (auth) *auth = (int)s_aps[idx].authmode;
    return true;
}
bool netcfg_wifi_connect(const char *ssid, const char *pass) {
    if (!s_started || !ssid || !ssid[0]) return false;
    s_connect_failed = false;
    save_creds(ssid, pass);
    start_sta(ssid, pass);
    return true;
}

static esp_err_t h_api_scan(httpd_req_t *req) {
    /* refresh when the cached list is empty or stale so the page is live */
    if (s_naps == 0 || esp_timer_get_time() - s_scan_us > SCAN_FRESH_US) do_scan();
    /* sorted indices by RSSI, skip hidden */
    int idx[SCAN_MAX], cnt = 0;
    for (int i = 0; i < s_naps; i++)
        if (s_aps[i].ssid[0]) idx[cnt++] = i;
    for (int i = 1; i < cnt; i++)
        for (int j = i; j > 0 && s_aps[idx[j]].rssi > s_aps[idx[j - 1]].rssi; j--) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    char b[1024];
    int n = snprintf(b, sizeof b, "[");
    for (int k = 0; k < cnt && n < (int)sizeof(b) - 80; k++) {
        wifi_ap_record_t *a = &s_aps[idx[k]];
        char es[40]; int e = 0;
        for (const char *p = (const char *)a->ssid; *p && e < 38; p++) {
            if (*p == '"' || *p == '\\') es[e++] = '\\';
            es[e++] = *p;
        }
        es[e] = 0;
        n += snprintf(b + n, sizeof(b) - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                      k ? "," : "", es, a->rssi, a->authmode != WIFI_AUTH_OPEN ? 1 : 0);
    }
    n += snprintf(b + n, sizeof(b) - n, "]");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
}

static esp_err_t h_api_rescan(httpd_req_t *req) {
    do_scan();
    return h_api_scan(req);
}

/* ---------------- pages / status ---------------- */
static const char *PAGE =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>RaceBrick WiFi</title></head>"
    "<body style=\"font-family:sans-serif;margin:22px;background:#111;color:#eee\">"
    "<h2 style=\"color:#4cf\">RaceBrick WiFi Setup</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>WIFI</label><br>"
    "<select id=\"ssid\" name=\"ssid\" onchange=\"document.getElementById('ssidm').value=''\" "
    "style=\"width:100%;padding:10px;font-size:16px\"></select>"
    "<input id=\"ssidm\" name=\"ssidm\" placeholder=\"or type WIFI name\" "
    "style=\"width:100%;padding:10px;font-size:16px;margin-top:8px\">"
    "<div style=\"margin:10px 0\"><button type=\"button\" onclick=\"rescan()\">Scan again</button></div>"
    "<label>Password</label><br>"
    "<input name=\"pass\" type=\"password\" style=\"width:100%;padding:10px;font-size:16px\"><br><br>"
    "<button style=\"padding:12px 22px;font-size:16px\">Connect</button>"
    "</form>"
    "<script>"
    "function fill(l){var s=document.getElementById('ssid');s.innerHTML='';"
    "if(!l.length){s.innerHTML='<option value=\"\">(no networks found)</option>';return;}"
    "l.forEach(function(a){var o=document.createElement('option');o.value=a.ssid;"
    "o.textContent=a.ssid+'  '+a.rssi+'dBm'+(a.auth?'  (locked)':'');s.appendChild(o);});}"
    "function rescan(){document.getElementById('ssid').innerHTML='<option>scanning...</option>';"
    "fetch('/api/rescan').then(r=>r.json()).then(fill).catch(function(){"
    "document.getElementById('ssid').innerHTML='<option>(scan failed)</option>';});}"
    "fetch('/api/scan').then(r=>r.json()).then(function(l){if(l&&l.length)fill(l);else rescan();}).catch(function(){});"
    "</script></body></html>";

static const char *SAVEPAGE =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
    "<body style=\"font-family:sans-serif;margin:22px;background:#111;color:#eee\">"
    "<h2 id=\"h\" style=\"color:#4cf\">Connecting...</h2>"
    "<p id=\"s\" style=\"font-size:16px\"></p>"
    "<p style=\"color:#888\">If it fails, check the password and try again.</p>"
    "<p><a href=\"/\" style=\"color:#9cf\">back</a></p>"
    "<script>"
    "function poll(){fetch('/status').then(r=>r.text()).then(function(t){"
    "document.getElementById('s').textContent=t;"
    "if(t.indexOf('CONNECTED')===0){document.getElementById('h').textContent='Connected';"
    "document.getElementById('s').textContent=t+'  ->  switch your phone to this WiFi, open http://'+t.split(' ').pop();}"
    "});}"
    "setInterval(poll,1000);poll();"
    "</script></body></html>";

static void urldecode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *d++ = *s++;
    }
    *d = 0;
}

static void getfield(const char *body, const char *key, char *out, int cap) {
    out[0] = 0;
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1;
            const char *end = strchr(p, '&');
            int n = end ? (int)(end - p) : (int)strlen(p);
            if (n >= cap) n = cap - 1;
            memcpy(out, p, n);
            out[n] = 0;
            urldecode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req) {
    char b[48];
    netcfg_status(b, sizeof b);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_save(httpd_req_t *req) {
    char body[256];
    int len = req->content_len;
    if (len >= (int)sizeof(body)) len = sizeof(body) - 1;
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[r] = 0;

    char ssid[33], ssidm[33], pass[65];
    getfield(body, "ssid", ssid, sizeof ssid);
    getfield(body, "ssidm", ssidm, sizeof ssidm);
    getfield(body, "pass", pass, sizeof pass);
    const char *use = ssidm[0] ? ssidm : ssid;
    if (!use[0]) { httpd_resp_send(req, "SSID required", HTTPD_RESP_USE_STRLEN); return ESP_OK; }

    save_creds(use, pass);
    start_sta(use, pass);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, SAVEPAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_api_status(httpd_req_t *req) {
    ESP_LOGI(TAG, "api status start");
    char b[384];
    gps_state_t *gp = gps_get();
    int n = snprintf(b, sizeof b,
        "{\"uid\":\"%s\",\"wifi\":%d,\"ip\":\"%s\",\"fix\":%d,\"sats\":%d,\"hdop\":%.1f,\"speed\":%.1f,"
        "\"lat\":%.6f,\"lon\":%.6f,\"lap\":%d,\"elapsed\":%.3f,\"tracks\":%d,"
        "\"nav\":%d,\"nav_age\":%d,\"nav_frames\":%d,\"nav_bytes\":%d,\"navz\":%d,\"nav_full\":%d,\"nav_mode\":%d}",
        cloud_sync_uid(), s_connected ? 1 : 0, s_ip, gp->fix_type, gp->sats_used, gp->hdop,
        gp->speed_kmh, gp->lat, gp->lon,
        g.engine_valid ? g.engine.lap : 0,
        g.engine_valid ? g.engine.elapsed : 0.0f, tstore_count(),
        nav_status(), nav_age_ms(), nav_frames(), nav_last_bytes(),
        nav_zoom_level(), g.nav_full, g.nav_mode);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    ESP_LOGI(TAG, "api status sent (%d): %s", n, esp_err_to_name(r));
    return r;
}

static void sha_hex(const uint8_t *data, char *out, int cap);

static esp_err_t h_api_v2_manifest(httpd_req_t *req) {
    char *b = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int n = snprintf(b, 32768,
        "{\"protocol\":2,\"device_id\":\"%s\",\"generated_at\":%lu,\"tracks\":[",
        cloud_sync_uid(), (unsigned long)time(NULL));
    int slots[TSTORE_EXT_SLOTS];
    int nslots = tstore_head_slots(slots, TSTORE_EXT_SLOTS);
    track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool first = true;
    if (t) {
        for (int i = 0; i < nslots && n < 26000; i++) {
            if (!tstore_load(slots[i], t)) continue;
            char tid[40], rid[40], pid[40];
            rb_uuid_format(&t->id, tid, sizeof(tid));
            rb_uuid_format(&t->rev_id, rid, sizeof(rid));
            rb_uuid_format(&t->parent_rev_id, pid, sizeof(pid));
            n += snprintf(b + n, 32768 - n,
                "%s{\"track_id\":\"%s\",\"local_slot\":%d,\"version\":%u,"
                "\"revision_id\":\"%s\",\"parent_revision_id\":\"%s\","
                "\"name\":\"%s\"}",
                first ? "" : ",", tid, slots[i], (unsigned) t->revision, rid, pid, t->name);
            first = false;
        }
        free(t);
    }
    n += snprintf(b + n, 32768 - n, "],\"sessions\":[");
    first = true;
    for (int i = 0; i < RACE_SLOTS && n < 30000; i++) {
        race_hdr_t h;
        if (!race_store_info(i, &h)) continue;
        char sid[40], tid[40], rid[40], sha[65];
        rb_uuid_format(&h.session_id, sid, sizeof(sid));
        rb_uuid_format(&h.track_id, tid, sizeof(tid));
        rb_uuid_format(&h.track_revision_id, rid, sizeof(rid));
        sha_hex(h.data_hash, sha, sizeof(sha));
        n += snprintf(b + n, 32768 - n,
            "%s{\"session_id\":\"%s\",\"local_slot\":%d,\"track_id\":\"%s\","
            "\"track_revision_id\":\"%s\",\"track_version\":%lu,"
            "\"track_name\":\"%s\",\"start\":%lu,\"end\":%lu,"
            "\"duration_ms\":%lu,\"laps\":%u,\"samples\":%lu,"
            "\"sample_size\":%u,\"content_hash\":\"%s\",\"telemetry_available\":true}",
            first ? "" : ",", sid, i, tid, rid, (unsigned long)h.track_revision,
            h.track_name, (unsigned long)h.start_epoch, (unsigned long)h.end_epoch,
            (unsigned long)h.duration_ms, h.lap_count,
            (unsigned long)h.sample_count,
            (unsigned)(h.sample_size ? h.sample_size : RACE_SAMPLE_SIZE), sha);
        first = false;
    }
    n += snprintf(b + n, 32768 - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static esp_err_t h_api_tracks(httpd_req_t *req) {
    ESP_LOGI(TAG, "api tracks start");
    int slots[TSTORE_EXT_SLOTS];
    int nslots = tstore_slots(slots, TSTORE_EXT_SLOTS);
    char *b = (char *)heap_caps_malloc(24000, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int n = snprintf(b, 24000, "{\"count\":%d,\"tracks\":[", nslots);
    bool first = true;
    track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) { free(b); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    for (int k = 0; k < nslots && n < 23000; k++) {
        int i = slots[k];
        if (!tstore_load(i, t)) continue;
        float sf = 0;
        if (t->geo && t->nscale > 1e-9f) {
            geo_ref_t r = { t->glat0, t->glon0 };
            float mx, my, sdist, ddist;
            geo_meters(&r, t->line_lat, t->line_lon, &mx, &my);
            pt_t q = { mx * t->nscale + t->nox, my * t->nscale + t->noy };
            if (track_project(t, q, &sdist, &ddist)) sf = sdist / (float)t->N;
        }
        char idbuf[40], revbuf[40];
        rb_uuid_format(&t->id, idbuf, sizeof(idbuf));
        rb_uuid_format(&t->rev_id, revbuf, sizeof(revbuf));
        n += snprintf(b + n, 24000 - n,
            "%s{\"id\":%d,\"uuid\":\"%s\",\"version\":%u,\"revision_id\":\"%s\","
            "\"name\":\"%s\",\"geo\":%d,\"m\":%.1f,"
            "\"sections\":%d,\"width\":%.1f,\"start_width\":%.1f,"
            "\"start_f\":%.4f,\"reverse\":%d,\"offset\":%.2f}",
            first ? "" : ",", i, idbuf, (unsigned)t->revision, revbuf,
            t->name, t->geo ? 1 : 0,
            (double)t->lengthM, t->nsec, (double)t->widthM,
            (double)t->start_width, (double)sf,
            t->reverse ? 1 : 0, (double)t->start_offset);
        first = false;
    }
    free(t);
    n += snprintf(b + n, 24000 - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    ESP_LOGI(TAG, "api tracks sent slots=%d len=%d: %s", nslots, n, esp_err_to_name(r));
    free(b);
    return r;
}

static int query_int(httpd_req_t *req, const char *name, int defv) {
    char q[512] = {0}, v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, name, v, sizeof v) == ESP_OK) {
        return atoi(v);
    }
    return defv;
}

static double query_double(httpd_req_t *req, const char *name, double defv) {
    char q[512] = {0}, v[40];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, name, v, sizeof v) == ESP_OK) {
        return strtod(v, NULL);
    }
    return defv;
}

static bool query_str(httpd_req_t *req, const char *name, char *out, int cap) {
    char q[512] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, name, out, cap) != ESP_OK) return false;
    urldecode(out);
    return true;
}

static esp_err_t h_api_track(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    if (req->method == HTTP_POST) {
        char name[64] = {0};
        bool has_name = query_str(req, "name", name, sizeof name) && name[0];
        double start_f = query_double(req, "start_f", -1.0);
        double width = query_double(req, "width", -1.0);
        double start_width = query_double(req, "start_width", -1.0);
        double offset = query_double(req, "offset", NAN);
        bool has_offset = isfinite(offset);
        int reverse = query_int(req, "reverse", -1);
        if (id < 0 || (!has_name && start_f < 0 && width < 0 && start_width < 0 &&
                       reverse < 0 && !has_offset)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id and a field required");
            return ESP_FAIL;
        }
        track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!t) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
        int old_slot = id;
        bool ok = tstore_load(id, t);
        if (ok) {
            char base_rev[40] = {0};
            if (query_str(req, "base_rev", base_rev, sizeof base_rev) && base_rev[0]) {
                rb_uuid_t base_uuid;
                if (!rb_uuid_parse(base_rev, &base_uuid)) {
                    free(t);
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad base_rev");
                    return ESP_FAIL;
                }
                if (!rb_uuid_equal(&t->rev_id, &base_uuid)) {
                    char cur[40];
                    rb_uuid_format(&t->rev_id, cur, sizeof(cur));
                    char resp[256];
                    snprintf(resp, sizeof(resp),
                             "{\"ok\":0,\"error\":\"version_conflict\","
                             "\"current_version\":%u,\"current_revision_id\":\"%s\"}",
                             (unsigned)t->revision, cur);
                    free(t);
                    httpd_resp_set_status(req, "409 Conflict");
                    httpd_resp_set_type(req, "application/json");
                    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
                }
            }
        }
        char old_name[48] = {0};
        if (ok) snprintf(old_name, sizeof old_name, "%s", t->name);
        int old_reverse = t ? t->reverse : 0;
        bool heading_updated = false;
        if (ok) {
            if (has_name) {
                snprintf(t->name, sizeof(t->name), "%s", name);
            }
            if (reverse >= 0) t->reverse = reverse ? 1 : 0;
            if (width > 0) {
                if (width < 1.0) width = 1.0;
                if (width > 50.0) width = 50.0;
                t->widthM = (float)width;
            }
            if (start_width > 0) {
                if (start_width < 1.0) start_width = 1.0;
                if (start_width > 50.0) start_width = 50.0;
                t->start_width = (float)start_width;
            }
            if (has_offset) {
                if (offset < -50.0) offset = -50.0;
                if (offset > 50.0) offset = 50.0;
                t->start_offset = (float)offset;
            }
            if ((start_f >= 0 || has_offset) && t->geo && t->nscale > 1e-9f) {
                if (start_f < 0) {
                    geo_ref_t cr = { t->glat0, t->glon0 };
                    float cmx, cmy, cs, cd;
                    geo_meters(&cr, t->line_lat, t->line_lon, &cmx, &cmy);
                    pt_t cq = { cmx * t->nscale + t->nox, cmy * t->nscale + t->noy };
                    if (track_project(t, cq, &cs, &cd)) start_f = cs / (float)t->N;
                }
                start_f = fmod(start_f, 1.0);
                if (start_f < 0) start_f += 1.0;
                pt_t q = track_point_at(t, (float)start_f);
                pt_t a = track_point_at(t, (float)start_f - 0.004f);
                pt_t b = track_point_at(t, (float)start_f + 0.004f);
                float hdg = atan2f(b.y - a.y, b.x - a.x);
                if (t->reverse) hdg += (float)M_PI;
                float offn = t->start_offset * t->nscale;
                q.x += sinf(hdg) * offn;
                q.y += -cosf(hdg) * offn;
                geo_ref_t r = { t->glat0, t->glon0 };
                double lat, lon;
                geo_from_meters(&r, (q.x - t->nox) / t->nscale, (q.y - t->noy) / t->nscale, &lat, &lon);
                t->line_lat = lat;
                t->line_lon = lon;
                t->line_hdg = hdg;
                heading_updated = true;
            }
            if (!heading_updated && reverse >= 0 && old_reverse != t->reverse) {
                t->line_hdg += (float)M_PI;
            }
            ok = tstore_commit(&id, t);
            if (ok && has_name && strcmp(old_name, t->name) != 0) {
                log_store_rename(old_name, t->name);
                race_store_rename(old_name, t->name);
                if (g.log) {
                    for (int j = 0; j < g.nlog; j++)
                        if (strcmp(g.log[j].trackName, old_name) == 0)
                            snprintf(g.log[j].trackName, sizeof(g.log[j].trackName), "%s", t->name);
                }
            }
            if (ok) {
                for (int j = 0; j < g.ntracks; j++) {
                    if (g.tracks[j].store_slot == old_slot) g.tracks[j] = *t;
                }
                if (g.engine_valid && g.engine.track && g.engine.track->store_slot == old_slot)
                    *g.engine.track = *t;
            }
        }
        char resp[256];
        if (ok) {
            char idbuf[40], revbuf[40];
            rb_uuid_format(&t->id, idbuf, sizeof(idbuf));
            rb_uuid_format(&t->rev_id, revbuf, sizeof(revbuf));
            snprintf(resp, sizeof(resp),
                     "{\"ok\":1,\"id\":%d,\"uuid\":\"%s\",\"version\":%u,"
                     "\"revision_id\":\"%s\"}",
                     id, idbuf, (unsigned)t->revision, revbuf);
        } else {
            snprintf(resp, sizeof(resp), "{\"ok\":0}");
        }
        free(t);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }
    if (req->method == HTTP_DELETE) {
        bool ok = id >= 0 && tstore_delete(id);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, ok ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
    }
    track_t *t = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!t) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    if (id < 0 || !tstore_load(id, t)) {
        free(t);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "track not found");
        return ESP_FAIL;
    }
    char *b = (char *)malloc(16000);
    if (!b) { free(t); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    float start_f = 0;
    if (t->geo && t->nscale > 1e-9f) {
        geo_ref_t r = { t->glat0, t->glon0 };
        float mx, my, sdist, ddist;
        geo_meters(&r, t->line_lat, t->line_lon, &mx, &my);
        pt_t q = { mx * t->nscale + t->nox, my * t->nscale + t->noy };
        if (track_project(t, q, &sdist, &ddist)) start_f = sdist / (float)t->N;
    }
    char idbuf[40], revbuf[40];
    rb_uuid_format(&t->id, idbuf, sizeof(idbuf));
    rb_uuid_format(&t->rev_id, revbuf, sizeof(revbuf));
    int n = snprintf(b, 16000,
        "{\"id\":%d,\"uuid\":\"%s\",\"version\":%u,\"revision_id\":\"%s\","
        "\"name\":\"%s\",\"geo\":%d,\"length_m\":%.1f,"
        "\"width\":%.1f,\"start_width\":%.1f,\"start_f\":%.4f,"
        "\"reverse\":%d,\"start_offset\":%.2f,"
        "\"start\":{\"lat\":%.7f,\"lon\":%.7f,\"hdg\":%.3f},"
        "\"glat0\":%.7f,\"glon0\":%.7f,\"nscale\":%.9f,"
        "\"nox\":%.6f,\"noy\":%.6f,\"sections\":[",
        id, idbuf, (unsigned)t->revision, revbuf,
        t->name, t->geo ? 1 : 0, (double)t->lengthM,
        (double)t->widthM, (double)t->start_width, (double)start_f,
        t->reverse ? 1 : 0, (double)t->start_offset,
        t->line_lat, t->line_lon, (double)t->line_hdg,
        t->glat0, t->glon0, (double)t->nscale, (double)t->nox, (double)t->noy);
    for (int i = 0; i < t->nsec && n < 15200; i++)
        n += snprintf(b + n, 16000 - n, "%s%.5f", i ? "," : "", (double)t->sections[i]);
    n += snprintf(b + n, 16000 - n, "],\"points\":[");
    for (int i = 0; i < t->N && n < 15300; i++)
        n += snprintf(b + n, 16000 - n, "%s%.4f,%.4f", i ? "," : "",
                      (double)t->points[i].x, (double)t->points[i].y);
    n += snprintf(b + n, 16000 - n, "]}");
    free(t);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static esp_err_t h_api_logs(httpd_req_t *req) {
    histrec_t *arr = (histrec_t *)heap_caps_malloc(
        sizeof(histrec_t) * HISTORY_RECORDS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *b = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arr || !b) { free(arr); free(b); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int nrec = log_store_load(arr, HISTORY_RECORDS);
    int n = snprintf(b, 32768, "{\"count\":%d,\"logs\":[", nrec);
    bool first = true;
    for (int k = nrec - 1; k >= 0 && n < 31000; k--) {
        histrec_t *r = &arr[k];
        n += snprintf(b + n, 32768 - n,
            "%s{\"index\":%d,\"track_name\":\"%s\",\"date\":\"%s\",\"mode\":%d,"
            "\"duration\":%.3f,\"laps\":%d,\"best\":%.3f,\"best_index\":%d,"
            "\"has_best\":%d,\"vehicle\":%d,\"lap_times\":[",
            first ? "" : ",", k, r->trackName, r->date, r->mode,
            (double)r->duration, r->lapCount, (double)r->bestTime,
            r->bestIndex, r->hasBest ? 1 : 0, r->vehicle);
        for (int i = 0; i < r->lapCount && i < HISTORY_LAPS; i++)
            n += snprintf(b + n, 32768 - n, "%s%.3f", i ? "," : "", (double)r->lapTime[i]);
        n += snprintf(b + n, 32768 - n, "]}");
        first = false;
    }
    n += snprintf(b + n, 32768 - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(arr); free(b);
    return r;
}

static esp_err_t h_api_log(httpd_req_t *req) {
    int idx = query_int(req, "id", -1);
    histrec_t *arr = (histrec_t *)heap_caps_malloc(
        sizeof(histrec_t) * HISTORY_RECORDS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *b = (char *)heap_caps_malloc(24576, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!arr || !b) { free(arr); free(b); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int nrec = log_store_load(arr, HISTORY_RECORDS);
    if (idx < 0 || idx >= nrec) { free(arr); free(b); httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "log not found"); return ESP_FAIL; }
    histrec_t *r = &arr[idx];
    int n = snprintf(b, 24576,
        "{\"index\":%d,\"track_name\":\"%s\",\"date\":\"%s\",\"mode\":%d,"
        "\"duration\":%.3f,\"laps\":%d,\"best\":%.3f,\"best_index\":%d,"
        "\"has_best\":%d,\"vehicle\":%d,\"lap_times\":[",
        idx, r->trackName, r->date, r->mode, (double)r->duration,
        r->lapCount, (double)r->bestTime, r->bestIndex, r->hasBest ? 1 : 0, r->vehicle);
    for (int i = 0; i < r->lapCount && i < HISTORY_LAPS; i++)
        n += snprintf(b + n, 24576 - n, "%s%.3f", i ? "," : "", (double)r->lapTime[i]);
    n += snprintf(b + n, 24576 - n, "],\"trace\":[");
    for (int i = 0; i < r->nTrace && n < 23000; i++)
        n += snprintf(b + n, 24576 - n, "%s%.5f,%.5f", i ? "," : "",
                      (double)r->trace[i].x, (double)r->trace[i].y);
    n += snprintf(b + n, 24576 - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t rr = httpd_resp_send(req, b, n);
    free(arr); free(b);
    return rr;
}

static esp_err_t h_api_log_rename(httpd_req_t *req) {
    char from[24] = {0}, to[48] = {0};
    if (!query_str(req, "from", from, sizeof from) ||
        !query_str(req, "to", to, sizeof to) || !from[0] || !to[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "from/to required");
        return ESP_FAIL;
    }
    int n = log_store_rename(from, to);
    char b[40];
    snprintf(b, sizeof b, "{\"ok\":1,\"changed\":%d}", n);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, HTTPD_RESP_USE_STRLEN);
}

static void sha_hex(const uint8_t *data, char *out, int cap) {
    if (cap < 65) { if (cap) out[0] = 0; return; }
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", data[i]);
    out[64] = 0;
}

static esp_err_t h_api_sessions(httpd_req_t *req) {
    const int cap = 8192;
    char *b = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int n = snprintf(b, cap, "{\"count\":%d,\"sessions\":[", race_store_count());
    bool first = true;
    for (int i = 0; i < RACE_SLOTS && n < cap - 240; i++) {
        race_hdr_t h;
        if (!race_store_info(i, &h)) continue;
        char sid[40], tid[40], rid[40], sha[65];
        rb_uuid_format(&h.session_id, sid, sizeof(sid));
        rb_uuid_format(&h.track_id, tid, sizeof(tid));
        rb_uuid_format(&h.track_revision_id, rid, sizeof(rid));
        sha_hex(h.data_hash, sha, sizeof(sha));
        n += snprintf(b + n, cap - n,
            "%s{\"slot\":%d,\"id\":%lu,\"session_uuid\":\"%s\","
            "\"track\":%u,\"track_uuid\":\"%s\",\"track_revision_id\":\"%s\","
            "\"track_version\":%lu,\"track_name\":\"%s\","
            "\"start\":%lu,\"end\":%lu,\"duration_ms\":%lu,"
            "\"samples\":%lu,\"sample_size\":%u,\"laps\":%u,\"mode\":%u,"
            "\"content_hash\":\"%s\"}",
            first ? "" : ",", i, (unsigned long)h.id, sid,
            h.track_slot, tid, rid, (unsigned long)h.track_revision,
            h.track_name, (unsigned long)h.start_epoch,
            (unsigned long)h.end_epoch, (unsigned long)h.duration_ms,
            (unsigned long)h.sample_count,
            (unsigned)(h.sample_size ? h.sample_size : RACE_SAMPLE_SIZE),
            h.lap_count, h.mode, sha);
        first = false;
    }
    n += snprintf(b + n, cap - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static esp_err_t h_api_session(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    if (req->method == HTTP_DELETE) {
        bool ok = id >= 0 && race_store_delete(id);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, ok ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
    }
    race_hdr_t h;
    if (id < 0 || !race_store_info(id, &h)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session not found");
        return ESP_FAIL;
    }
    const int cap = 4096;
    char *b = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    char sid[40], tid[40], rid[40], sha[65];
    rb_uuid_format(&h.session_id, sid, sizeof(sid));
    rb_uuid_format(&h.track_id, tid, sizeof(tid));
    rb_uuid_format(&h.track_revision_id, rid, sizeof(rid));
    sha_hex(h.data_hash, sha, sizeof(sha));
    int n = snprintf(b, cap,
        "{\"slot\":%d,\"id\":%lu,\"session_uuid\":\"%s\","
        "\"track\":%u,\"track_uuid\":\"%s\",\"track_revision_id\":\"%s\","
        "\"track_version\":%lu,\"track_name\":\"%s\","
        "\"start\":%lu,\"end\":%lu,\"duration_ms\":%lu,"
        "\"samples\":%lu,\"sample_size\":%u,\"laps\":%u,\"deleted_laps\":%u,"
        "\"content_hash\":\"%s\",\"lap_times\":[", id, (unsigned long)h.id, sid,
        h.track_slot, tid, rid, (unsigned long)h.track_revision, h.track_name,
        (unsigned long)h.start_epoch, (unsigned long)h.end_epoch,
        (unsigned long)h.duration_ms, (unsigned long)h.sample_count,
        (unsigned)(h.sample_size ? h.sample_size : RACE_SAMPLE_SIZE),
        h.lap_count, h.lap_deleted_mask, sha);
    for (int i = 0; i < h.lap_count && n < cap - 1280; i++)
        n += snprintf(b + n, cap - n, "%s%.3f", i ? "," : "", (double)h.lap_time[i]);
    n += snprintf(b + n, cap - n, "],\"lap_ids\":[");
    for (int i = 0; i < h.lap_count && n < cap - 800; i++) {
        char lid[40];
        rb_uuid_format(&h.lap_id[i], lid, sizeof(lid));
        n += snprintf(b + n, cap - n, "%s\"%s\"", i ? "," : "", lid);
    }
    n += snprintf(b + n, cap - n, "],\"lap_sample_start\":[");
    for (int i = 0; i < h.lap_count && n < cap - 400; i++)
        n += snprintf(b + n, cap - n, "%s%lu", i ? "," : "",
                      (unsigned long)h.lap_sample_start[i]);
    n += snprintf(b + n, cap - n, "],\"lap_sample_end\":[");
    for (int i = 0; i < h.lap_count && n < cap - 180; i++)
        n += snprintf(b + n, cap - n, "%s%lu", i ? "," : "",
                      (unsigned long)h.lap_sample_end[i]);
    n += snprintf(b + n, cap - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static esp_err_t h_api_session_lap_delete(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    int lap = query_int(req, "lap", -1);
    bool ok = id >= 0 && lap > 0 && race_store_delete_lap(id, lap);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

static int64_t session_lap1_cutoff_ms(int id, const race_hdr_t *h) {
    if (h->lap_count < 1 || h->lap_time[0] <= 0.0f) return 0;
    int64_t last = -1;
    for (uint32_t i = 0; i < h->sample_count; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(id, (int)i, &sm)) break;
        if (sm.lap == 1 && (sm.flags & 1)) last = sm.t_ms;
    }
    if (last < 0) return 0;
    int64_t cut = last - (int64_t)(h->lap_time[0] * 1000.0f);
    return cut > 0 ? cut : 0;
}

static esp_err_t h_api_session_laps(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    race_hdr_t h;
    if (id < 0 || !race_store_info(id, &h)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session not found");
        return ESP_FAIL;
    }
    int64_t l1cut = session_lap1_cutoff_ms(id, &h);
    uint32_t cnt[RACE_LAPS] = {0};
    double dist[RACE_LAPS] = {0};
    double sum_kmh[RACE_LAPS] = {0};
    float max_kmh[RACE_LAPS] = {0};
    double prev_lat = 0, prev_lon = 0;
    int prev_lap = -1;
    for (uint32_t i = 0; i < h.sample_count; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(id, (int)i, &sm)) break;
        if (sm.lap < 1 || sm.lap > RACE_LAPS) continue;
        if (sm.lap == 1 && (int64_t)sm.t_ms < l1cut) continue;
        int lap = sm.lap - 1;
        if (!(sm.flags & 1)) continue;
        double lat = sm.lat_e7 / 1e7, lon = sm.lon_e7 / 1e7;
        float kmh = sm.speed10 / 10.0f;
        cnt[lap]++;
        sum_kmh[lap] += kmh;
        if (kmh > max_kmh[lap]) max_kmh[lap] = kmh;
        if (prev_lap == lap) {
            double dlat = (lat - prev_lat) * 111320.0;
            double dlon = (lon - prev_lon) * 111320.0 * cos(lat * M_PI / 180.0);
            dist[lap] += sqrt(dlat * dlat + dlon * dlon);
        }
        prev_lat = lat; prev_lon = lon; prev_lap = lap;
    }
    const int cap = 4096;
    char *b = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int n = snprintf(b, cap, "{\"slot\":%d,\"laps\":[", id);
    bool first = true;
    for (int i = 0; i < h.lap_count && i < RACE_LAPS; i++) {
        bool deleted = (h.lap_deleted_mask & (1u << i)) != 0;
        double avg = cnt[i] ? sum_kmh[i] / cnt[i] : 0.0;
        n += snprintf(b + n, cap - n,
            "%s{\"lap\":%d,\"time\":%.3f,\"samples\":%lu,\"distance_m\":%.1f,"
            "\"avg_kmh\":%.1f,\"max_kmh\":%.1f,\"deleted\":%d}",
            first ? "" : ",", i + 1, (double)h.lap_time[i],
            (unsigned long)cnt[i], dist[i], avg, (double)max_kmh[i], deleted ? 1 : 0);
        first = false;
    }
    n += snprintf(b + n, cap - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, b, n);
    free(b);
    return r;
}

static int append_lap_arrays(char *b, int n, int cap,
                             const float *sp, const uint32_t *tm, int cnt) {
    n += snprintf(b + n, cap - n, "],\"speeds\":[");
    for (int i = 0; i < cnt && n < cap - 32; i++)
        n += snprintf(b + n, cap - n, "%s%.1f", i ? "," : "", (double)sp[i]);
    n += snprintf(b + n, cap - n, "],\"times\":[");
    for (int i = 0; i < cnt && n < cap - 32; i++)
        n += snprintf(b + n, cap - n, "%s%lu", i ? "," : "", (unsigned long)tm[i]);
    n += snprintf(b + n, cap - n, "]}");
    return n;
}

static esp_err_t h_api_session_traces(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    race_hdr_t h;
    if (id < 0 || !race_store_info(id, &h)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session not found");
        return ESP_FAIL;
    }
    track_t *tr = (track_t *)heap_caps_malloc(sizeof(track_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool have_tr = tr && h.track_slot < TSTORE_EXT_SLOTS && tstore_load(h.track_slot, tr) && tr->geo;
    int64_t l1cut = session_lap1_cutoff_ms(id, &h);
    double lat0 = 0, lon0 = 0;
    bool first_geo = true;
    uint32_t cnt[RACE_LAPS] = {0};
    int step[RACE_LAPS];
    for (uint32_t i = 0; i < h.sample_count; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(id, (int)i, &sm)) break;
        if (sm.lap < 1 || sm.lap > h.lap_count) continue;
        if (!(sm.flags & 1)) continue;
        if (sm.lap == 1 && (int64_t)sm.t_ms < l1cut) continue;
        if (first_geo) { lat0 = sm.lat_e7 / 1e7; lon0 = sm.lon_e7 / 1e7; first_geo = false; }
        cnt[sm.lap - 1]++;
    }
    for (int i = 0; i < RACE_LAPS; i++) step[i] = cnt[i] > 96 ? (int)((cnt[i] + 95) / 96) : 1;
    char *b = (char *)heap_caps_malloc(32768, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { free(tr); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    int n = snprintf(b, 32768, "{\"slot\":%d,\"laps\":[", id);
    bool first_lap = true;
    int cur_lap = -1;
    bool first_point = true;
    int seen[RACE_LAPS] = {0};
    float *cur_sp = (float *)heap_caps_malloc(sizeof(float) * 128,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t *cur_tm = (uint32_t *)heap_caps_malloc(sizeof(uint32_t) * 128,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cur_sp || !cur_tm) {
        free(cur_sp); free(cur_tm); free(b); free(tr);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int cur_n = 0;
    for (uint32_t i = 0; i < h.sample_count && n < 28000; i++) {
        race_sample_t sm;
        if (!race_store_read_sample(id, (int)i, &sm)) break;
        if (sm.lap < 1 || sm.lap > h.lap_count) continue;
        if (!(sm.flags & 1)) continue;
        if (sm.lap == 1 && (int64_t)sm.t_ms < l1cut) continue;
        if (h.lap_deleted_mask & (1u << (sm.lap - 1))) continue;
        int li = sm.lap - 1;
        if ((seen[li]++ % step[li]) != 0) continue;
        double lat = sm.lat_e7 / 1e7, lon = sm.lon_e7 / 1e7;
        float x, y;
        if (have_tr) {
            pt_t q = track_geo_to_norm(tr, lat, lon);
            x = q.x; y = q.y;
        } else {
            x = (float)((lon - lon0) * 111320.0 * cos(lat0 * M_PI / 180.0));
            y = (float)((lat - lat0) * 111320.0);
        }
        if (sm.lap != cur_lap) {
            if (cur_lap >= 0) n = append_lap_arrays(b, n, 32768, cur_sp, cur_tm, cur_n);
            if (!first_lap) n += snprintf(b + n, 32768 - n, ",");
            n += snprintf(b + n, 32768 - n, "{\"lap\":%d,\"points\":[", sm.lap);
            cur_lap = sm.lap;
            first_lap = false;
            first_point = true;
            cur_n = 0;
        }
        n += snprintf(b + n, 32768 - n, "%s%.5f,%.5f", first_point ? "" : ",",
                      (double)x, (double)y);
        first_point = false;
        if (cur_n < 128) {
            cur_sp[cur_n] = sm.speed10 / 10.0f;
            cur_tm[cur_n] = sm.t_ms;
            cur_n++;
        }
    }
    if (cur_lap >= 0) n = append_lap_arrays(b, n, 32768, cur_sp, cur_tm, cur_n);
    n += snprintf(b + n, 32768 - n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t rr = httpd_resp_send(req, b, n);
    free(cur_sp); free(cur_tm); free(b); free(tr);
    return rr;
}

static esp_err_t h_api_session_file(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    race_hdr_t h;
    if (id < 0 || !race_store_info(id, &h)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session not found");
        return ESP_FAIL;
    }
    int64_t l1cut = session_lap1_cutoff_ms(id, &h);
    httpd_resp_set_type(req, "text/csv");
    char line[512];
    bool v2 = h.ver >= 2 && h.sample_size == RACE_SAMPLE_V2_SIZE;
    int n = snprintf(line, sizeof line,
        "Format,3\nSession title,\"%s\"\nSession type,Lap timing\nTrack name,\"%s\"\n"
        "timestamp,fragment_id,lap_number,elapsed_time,latitude,longitude,speed%s\n"
        "unix time,,,s,deg,deg,m/s%s\n"
        ",,,,100: gps,100: gps,100: gps%s\n", h.track_name, h.track_name,
        v2 ? ",gps_accuracy_m,satellites,fix_type" : "",
        v2 ? ",m,,count" : "",
        v2 ? ",100: gps,100: gps,100: gps" : "");
    httpd_resp_send_chunk(req, line, n);
    for (uint32_t i = 0; i < h.sample_count; i++) {
        race_sample_v2_t sm;
        if (!race_store_read_sample_v2(id, (int)i, &sm)) break;
        if (sm.lap == 0 || sm.lap > h.lap_count) continue;
        if (sm.lap == 1 && (int64_t)sm.t_ms < l1cut) continue;
        if (h.lap_deleted_mask & (1u << (sm.lap - 1))) continue;
        if (v2) {
            n = snprintf(line, sizeof line, "%.3f,0,%u,%.3f,%.7f,%.7f,%.3f,%.2f,%u,%u\n",
                         (double)h.start_epoch + sm.t_ms / 1000.0, sm.lap, sm.t_ms / 1000.0,
                         sm.lat_e7 / 1e7, sm.lon_e7 / 1e7, sm.speed10 / 36.0,
                         sm.accuracy_cm == 0xFFFFu ? -1.0 : sm.accuracy_cm / 100.0,
                         sm.sats, sm.fix_type);
        } else {
            n = snprintf(line, sizeof line, "%.3f,0,%u,%.3f,%.7f,%.7f,%.3f\n",
                         (double)h.start_epoch + sm.t_ms / 1000.0, sm.lap, sm.t_ms / 1000.0,
                         sm.lat_e7 / 1e7, sm.lon_e7 / 1e7, sm.speed10 / 36.0);
        }
        httpd_resp_send_chunk(req, line, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t h_api_session_chunk(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    int seq = query_int(req, "seq", 0);
    int size = query_int(req, "size", 2000);
    race_hdr_t h;
    if (id < 0 || !race_store_info(id, &h)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session not found");
        return ESP_FAIL;
    }
    if (seq < 0 || size < 1 || size > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad chunk");
        return ESP_FAIL;
    }
    uint32_t start = (uint32_t)seq * (uint32_t)size;
    if (start >= h.sample_count) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "beyond end");
        return ESP_FAIL;
    }
    uint32_t end = start + (uint32_t)size;
    if (end > h.sample_count) end = h.sample_count;
    char hdr[48];
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)start);
    httpd_resp_set_hdr(req, "X-Sample-Start", hdr);
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)end);
    httpd_resp_set_hdr(req, "X-Sample-End", hdr);
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)h.sample_count);
    httpd_resp_set_hdr(req, "X-Sample-Total", hdr);
    snprintf(hdr, sizeof(hdr), "%d", seq);
    httpd_resp_set_hdr(req, "X-Chunk-Seq", hdr);

    size_t cap = (size_t)size * 128u + 1024u;
    char *out = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    bool v2 = h.ver >= 2 && h.sample_size == RACE_SAMPLE_V2_SIZE;
    int n = 0;
    char line[320];
    for (uint32_t i = start; i < end && (size_t)n < cap - 340; i++) {
        race_sample_v2_t sm;
        if (!race_store_read_sample_v2(id, (int)i, &sm)) break;
        if (sm.lap == 0 || sm.lap > h.lap_count) continue;
        if (h.lap_deleted_mask & (1u << (sm.lap - 1))) continue;
        int ln;
        if (v2) {
            ln = snprintf(line, sizeof(line),
                "%.3f,0,%u,%.3f,%.7f,%.7f,%.3f,%.2f,%u,%u\n",
                (double)h.start_epoch + sm.t_ms / 1000.0, sm.lap, sm.t_ms / 1000.0,
                sm.lat_e7 / 1e7, sm.lon_e7 / 1e7, sm.speed10 / 36.0,
                sm.accuracy_cm == 0xFFFFu ? -1.0 : sm.accuracy_cm / 100.0,
                sm.sats, sm.fix_type);
        } else {
            ln = snprintf(line, sizeof(line),
                "%.3f,0,%u,%.3f,%.7f,%.7f,%.3f\n",
                (double)h.start_epoch + sm.t_ms / 1000.0, sm.lap, sm.t_ms / 1000.0,
                sm.lat_e7 / 1e7, sm.lon_e7 / 1e7, sm.speed10 / 36.0);
        }
        memcpy(out + n, line, (size_t)ln);
        n += ln;
    }
    unsigned char digest[32];
    size_t digest_len = 0;
    psa_crypto_init();
    psa_status_t hs = psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)out,
                                       (size_t)n, digest, sizeof(digest), &digest_len);
    if (hs != PSA_SUCCESS || digest_len != sizeof(digest)) {
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sha256");
        return ESP_FAIL;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[64] = 0;
    httpd_resp_set_hdr(req, "X-Chunk-SHA256", hex);
    httpd_resp_set_type(req, "text/csv");
    esp_err_t r = httpd_resp_send(req, out, n);
    free(out);
    return r;
}

static esp_err_t h_api_transfer(httpd_req_t *req) {
    char q[64] = {0}, v[16] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "state", v, sizeof v) == ESP_OK) {
        if (!strcmp(v, "begin")) netcfg_transfer_begin();
        else if (!strcmp(v, "end")) netcfg_transfer_end();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_api_control(httpd_req_t *req) {
    char body[64];
    int len = req->content_len;
    if (len >= (int)sizeof(body)) len = sizeof(body) - 1;
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    body[r] = 0;
    int cmd = 0;
    if (strstr(body, "cmd=home")) cmd = 1;
    else if (strstr(body, "cmd=perf")) cmd = 2;
    else if (strstr(body, "cmd=race")) cmd = 3;
    else if (strstr(body, "cmd=debug")) cmd = 4;
    else if (strstr(body, "cmd=nav") || strstr(body, "cmd=location")) cmd = 5;
    if (cmd) {
        taskENTER_CRITICAL(&s_mux);
        s_cmd = cmd;
        taskEXIT_CRITICAL(&s_mux);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, cmd ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

/* ---- debug/inspection endpoints ---- */
static esp_err_t h_api_gps(httpd_req_t *req) {
    char q[160] = {0}, v[32];
    double lat = 0, lon = 0;
    float spd = 0, crs = 0;
    bool ok = false;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "lat", v, sizeof v) == ESP_OK) { lat = strtod(v, NULL); ok = true; }
        if (httpd_query_key_value(q, "lon", v, sizeof v) == ESP_OK) { lon = strtod(v, NULL); ok = true; }
        if (httpd_query_key_value(q, "speed", v, sizeof v) == ESP_OK) spd = (float)strtod(v, NULL);
        if (httpd_query_key_value(q, "course", v, sizeof v) == ESP_OK) crs = (float)strtod(v, NULL);
    }
    if (ok) {
        gps_state_t *gp = gps_get();
        gp->lat = lat; gp->lon = lon;
        gp->has_pos = true;
        gp->fix_type = 2; gp->fix_quality = 1;
        gp->sats_used = 12; gp->hdop = 0.8f;
        gp->speed_kmh = spd; gp->course = crs;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_api_key(httpd_req_t *req) {
    char q[32] = {0}, v[12];
    int k = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "b", v, sizeof v) == ESP_OK) {
        if (!strcmp(v, "up")) k = 1;
        else if (!strcmp(v, "down")) k = 2;
        else if (!strcmp(v, "push")) k = 3;
        else if (!strcmp(v, "back")) k = 4;
    }
    if (k) {
        taskENTER_CRITICAL(&s_mux);
        s_key = k;
        taskEXIT_CRITICAL(&s_mux);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, k ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_api_goto(httpd_req_t *req) {
    char q[64] = {0}, v[16];
    int n = -1, e = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
        if (httpd_query_key_value(q, "n", v, sizeof v) == ESP_OK) n = atoi(v);
        if (httpd_query_key_value(q, "e", v, sizeof v) == ESP_OK) e = atoi(v);
    }
    if (n < 0 || n >= SC_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad n");
        return ESP_FAIL;
    }
    taskENTER_CRITICAL(&s_mux);
    s_goto = n; s_goto_enter = e;
    taskEXIT_CRITICAL(&s_mux);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":1}", HTTPD_RESP_USE_STRLEN);
}

/* raw 320x240 RGB565 framebuffer (native little-endian) for UI inspection.
 * ?f=1 freezes the UI (no redraw) for a tear-free dump; ?f=0 unfreezes. */
static esp_err_t h_api_fb(httpd_req_t *req) {
    char q[32] = {0}, v[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "f", v, sizeof v) == ESP_OK) {
        if (atoi(v) == 1) {
            g_ui_freeze = true;
            vTaskDelay(pdMS_TO_TICKS(80));   /* let the in-flight frame finish */
        } else {
            g_ui_freeze = false;
            httpd_resp_set_type(req, "text/plain");
            return httpd_resp_send(req, "ok", 2);
        }
    }
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)gfx_fb(), LCD_W * LCD_H * 2);
}

static esp_err_t h_api_track_image(httpd_req_t *req) {
    int id = query_int(req, "id", -1);
    double x0 = query_double(req, "x0", NAN);
    double y0 = query_double(req, "y0", NAN);
    double x1 = query_double(req, "x1", NAN);
    double y1 = query_double(req, "y1", NAN);
    int len = req->content_len;
    if (id < 0 || len <= 6 || len > 127000 ||
        !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        x1 <= x0 || y1 <= y0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image request");
        return ESP_FAIL;
    }
    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)len,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, (char *)buf + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    bool ok = false;
    if (got == len)
        ok = track_img_save(id, buf, len, (float)x0, (float)y0, (float)x1, (float)y1);
    free(buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, ok ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

static void start_httpd(void) {
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;
    cfg.max_uri_handlers = 32;   /* UI + debug + race storage + satellite */
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { s_httpd = NULL; return; }
    httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET, .handler = h_index };
    httpd_uri_t u2 = { .uri = "/status", .method = HTTP_GET, .handler = h_status };
    httpd_uri_t u3 = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u4 = { .uri = "/api/status", .method = HTTP_GET, .handler = h_api_status };
    httpd_uri_t u5 = { .uri = "/api/tracks", .method = HTTP_GET, .handler = h_api_tracks };
    httpd_uri_t u6 = { .uri = "/api/control", .method = HTTP_POST, .handler = h_api_control };
    httpd_uri_t u7 = { .uri = "/api/scan", .method = HTTP_GET, .handler = h_api_scan };
    httpd_uri_t u8 = { .uri = "/api/rescan", .method = HTTP_GET, .handler = h_api_rescan };
    httpd_uri_t u9 = { .uri = "/api/goto", .method = HTTP_GET, .handler = h_api_goto };
    httpd_uri_t u10 = { .uri = "/api/fb", .method = HTTP_GET, .handler = h_api_fb };
    httpd_uri_t u11 = { .uri = "/api/key", .method = HTTP_GET, .handler = h_api_key };
    httpd_uri_t u12 = { .uri = "/api/gps", .method = HTTP_GET, .handler = h_api_gps };
    httpd_uri_t u13 = { .uri = "/api/track", .method = HTTP_GET, .handler = h_api_track };
    httpd_uri_t u14 = { .uri = "/api/track", .method = HTTP_DELETE, .handler = h_api_track };
    httpd_uri_t u15 = { .uri = "/api/sessions", .method = HTTP_GET, .handler = h_api_sessions };
    httpd_uri_t u16 = { .uri = "/api/session", .method = HTTP_GET, .handler = h_api_session };
    httpd_uri_t u17 = { .uri = "/api/session", .method = HTTP_DELETE, .handler = h_api_session };
    httpd_uri_t u18 = { .uri = "/api/session/lap", .method = HTTP_DELETE, .handler = h_api_session_lap_delete };
    httpd_uri_t u19 = { .uri = "/api/session/file", .method = HTTP_GET, .handler = h_api_session_file };
    httpd_uri_t u20 = { .uri = "/api/track", .method = HTTP_POST, .handler = h_api_track };
    httpd_uri_t u21 = { .uri = "/api/session/laps", .method = HTTP_GET, .handler = h_api_session_laps };
    httpd_uri_t u22 = { .uri = "/api/logs", .method = HTTP_GET, .handler = h_api_logs };
    httpd_uri_t u23 = { .uri = "/api/log", .method = HTTP_GET, .handler = h_api_log };
    httpd_uri_t u24 = { .uri = "/api/log/rename", .method = HTTP_POST, .handler = h_api_log_rename };
    httpd_uri_t u25 = { .uri = "/api/session/traces", .method = HTTP_GET, .handler = h_api_session_traces };
    httpd_uri_t u26 = { .uri = "/api/track/image", .method = HTTP_POST, .handler = h_api_track_image };
    httpd_uri_t u27 = { .uri = "/api/v2/manifest", .method = HTTP_GET, .handler = h_api_v2_manifest };
    httpd_uri_t u28 = { .uri = "/api/session/chunk", .method = HTTP_GET, .handler = h_api_session_chunk };
    httpd_uri_t u29 = { .uri = "/api/transfer", .method = HTTP_POST, .handler = h_api_transfer };
    httpd_register_uri_handler(s_httpd, &u1);
    httpd_register_uri_handler(s_httpd, &u2);
    httpd_register_uri_handler(s_httpd, &u3);
    httpd_register_uri_handler(s_httpd, &u4);
    httpd_register_uri_handler(s_httpd, &u5);
    httpd_register_uri_handler(s_httpd, &u6);
    httpd_register_uri_handler(s_httpd, &u7);
    httpd_register_uri_handler(s_httpd, &u8);
    httpd_register_uri_handler(s_httpd, &u9);
    httpd_register_uri_handler(s_httpd, &u10);
    httpd_register_uri_handler(s_httpd, &u11);
    httpd_register_uri_handler(s_httpd, &u12);
    httpd_register_uri_handler(s_httpd, &u13);
    httpd_register_uri_handler(s_httpd, &u14);
    httpd_register_uri_handler(s_httpd, &u15);
    httpd_register_uri_handler(s_httpd, &u16);
    httpd_register_uri_handler(s_httpd, &u17);
    httpd_register_uri_handler(s_httpd, &u18);
    httpd_register_uri_handler(s_httpd, &u19);
    httpd_register_uri_handler(s_httpd, &u20);
    httpd_register_uri_handler(s_httpd, &u21);
    httpd_register_uri_handler(s_httpd, &u22);
    httpd_register_uri_handler(s_httpd, &u23);
    httpd_register_uri_handler(s_httpd, &u24);
    httpd_register_uri_handler(s_httpd, &u25);
    httpd_register_uri_handler(s_httpd, &u26);
    httpd_register_uri_handler(s_httpd, &u27);
    httpd_register_uri_handler(s_httpd, &u28);
    httpd_register_uri_handler(s_httpd, &u29);
}

void netcfg_start_ap(void) {
    if (s_started) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_evt, NULL, NULL);

    esp_wifi_set_mode(WIFI_MODE_APSTA);

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "RaceBrick-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    setstr(ap.ap.ssid, sizeof(ap.ap.ssid), s_ap_ssid);
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    setstr(ap.ap.password, sizeof(ap.ap.password), "racebrick");
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    char ssid[33], pass[65];
    bool have_creds = load_creds(ssid, sizeof ssid, pass, sizeof pass);

    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    esp_err_t tx = esp_wifi_set_max_tx_power(WIFI_AP_TX_POWER_QDBM);
    ESP_LOGI(TAG, "AP TX power set %d qdBm: %s", WIFI_AP_TX_POWER_QDBM, esp_err_to_name(tx));

    /* No boot-time AP scan: /api/scan performs it on demand for WiFi Setup.
     * Scanning here blocked the first UI frame for ~3 s. */
    start_httpd();
    ESP_LOGI(TAG, "SoftAP '%s' up, open http://192.168.4.1 (scan: %d APs)", s_ap_ssid, s_naps);

    if (have_creds) start_sta(ssid, pass);   /* connect after the scan */
}
