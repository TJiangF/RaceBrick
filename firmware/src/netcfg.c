#include "netcfg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "gps.h"
#include "tstore.h"

static const char *TAG = "netcfg";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_cmd = 0;

int netcfg_take_cmd(void) {
    int c;
    taskENTER_CRITICAL(&s_mux);
    c = s_cmd; s_cmd = 0;
    taskEXIT_CRITICAL(&s_mux);
    return c;
}

static httpd_handle_t s_httpd = NULL;
static bool s_started = false;
static bool s_connecting = false;
static bool s_connected = false;
static int64_t s_connect_t0 = 0;
static char s_ip[20] = "";
static char s_ap_ssid[24] = "RaceBrick";

#define SCAN_MAX 16
static wifi_ap_record_t s_aps[SCAN_MAX];
static int s_naps = 0;
static int64_t s_scan_us = 0;
#define SCAN_FRESH_US 3000000   /* reuse a scan up to 3 s old */

bool netcfg_running(void) { return s_started; }
bool netcfg_connected(void) { return s_connected; }
const char *netcfg_ip(void) { return s_ip; }
const char *netcfg_ap_ssid(void) { return s_ap_ssid; }

void netcfg_status(char *buf, int cap) {
    if (s_connected) {
        snprintf(buf, cap, "CONNECTED %s", s_ip);
    } else if (s_connecting) {
        int sec = (int)((esp_timer_get_time() - s_connect_t0) / 1000000);
        if (sec > 20) snprintf(buf, cap, "FAILED - CHECK SSID/PASSWORD");
        else snprintf(buf, cap, "CONNECTING... %ds", sec);
    } else if (s_started) {
        snprintf(buf, cap, "AP: %s", s_ap_ssid);
    } else {
        snprintf(buf, cap, "NOT STARTED");
    }
}

/* ---------------- credentials ---------------- */
static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

static bool load_creds(char *ssid, int sc, char *pass, int pc) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = (size_t)sc;
    bool ok = (nvs_get_str(h, "ssid", ssid, &l) == ESP_OK) && ssid[0];
    l = (size_t)pc;
    if (ok) ok = (nvs_get_str(h, "pass", pass, &l) == ESP_OK);
    nvs_close(h);
    return ok;
}

bool netcfg_has_creds(void) {
    char ssid[33], pass[65];
    return load_creds(ssid, sizeof ssid, pass, sizeof pass);
}

static void setstr(uint8_t *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void start_sta(const char *ssid, const char *pass) {
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    setstr(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    setstr(sta.sta.password, sizeof(sta.sta.password), pass);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
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
            if (s_connecting) esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_connecting = false;
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
    char b[320];
    gps_state_t *gp = gps_get();
    int n = snprintf(b, sizeof b,
        "{\"wifi\":%d,\"ip\":\"%s\",\"fix\":%d,\"sats\":%d,\"hdop\":%.1f,\"speed\":%.1f,"
        "\"lat\":%.6f,\"lon\":%.6f,\"lap\":%d,\"elapsed\":%.3f,\"tracks\":%d}",
        s_connected ? 1 : 0, s_ip, gp->fix_type, gp->sats_used, gp->hdop,
        gp->speed_kmh, gp->lat, gp->lon,
        g.engine_valid ? g.engine.lap : 0,
        g.engine_valid ? g.engine.elapsed : 0.0f, tstore_count());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
}

static esp_err_t h_api_tracks(httpd_req_t *req) {
    char b[512];
    int n = snprintf(b, sizeof b, "{\"count\":%d,\"tracks\":[", tstore_count());
    for (int i = 0; i < g.ntracks && n < (int)sizeof(b) - 48; i++) {
        n += snprintf(b + n, sizeof(b) - n, "%s{\"name\":\"%s\",\"geo\":%d,\"m\":%.1f}",
                      i ? "," : "", g.tracks[i].name, g.tracks[i].geo ? 1 : 0,
                      (double)g.tracks[i].lengthM);
    }
    n += snprintf(b + n, sizeof(b) - n, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, b, n);
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
    if (cmd) {
        taskENTER_CRITICAL(&s_mux);
        s_cmd = cmd;
        taskEXIT_CRITICAL(&s_mux);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, cmd ? "{\"ok\":1}" : "{\"ok\":0}", HTTPD_RESP_USE_STRLEN);
}

static void start_httpd(void) {
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 4096;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { s_httpd = NULL; return; }
    httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET, .handler = h_index };
    httpd_uri_t u2 = { .uri = "/status", .method = HTTP_GET, .handler = h_status };
    httpd_uri_t u3 = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
    httpd_uri_t u4 = { .uri = "/api/status", .method = HTTP_GET, .handler = h_api_status };
    httpd_uri_t u5 = { .uri = "/api/tracks", .method = HTTP_GET, .handler = h_api_tracks };
    httpd_uri_t u6 = { .uri = "/api/control", .method = HTTP_POST, .handler = h_api_control };
    httpd_uri_t u7 = { .uri = "/api/scan", .method = HTTP_GET, .handler = h_api_scan };
    httpd_uri_t u8 = { .uri = "/api/rescan", .method = HTTP_GET, .handler = h_api_rescan };
    httpd_register_uri_handler(s_httpd, &u1);
    httpd_register_uri_handler(s_httpd, &u2);
    httpd_register_uri_handler(s_httpd, &u3);
    httpd_register_uri_handler(s_httpd, &u4);
    httpd_register_uri_handler(s_httpd, &u5);
    httpd_register_uri_handler(s_httpd, &u6);
    httpd_register_uri_handler(s_httpd, &u7);
    httpd_register_uri_handler(s_httpd, &u8);
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

    /* scan while the STA is idle (scanning while connecting returns nothing) */
    do_scan();
    start_httpd();
    ESP_LOGI(TAG, "SoftAP '%s' up, open http://192.168.4.1 (scan: %d APs)", s_ap_ssid, s_naps);

    if (have_creds) start_sta(ssid, pass);   /* connect after the scan */
}
