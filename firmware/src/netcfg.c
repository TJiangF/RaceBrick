#include "netcfg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
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
static char s_ip[20] = "";
static char s_ap_ssid[24] = "RaceBrick";

static void setstr(uint8_t *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool netcfg_running(void) { return s_started; }
bool netcfg_connected(void) { return s_connected; }
const char *netcfg_ip(void) { return s_ip; }
const char *netcfg_ap_ssid(void) { return s_ap_ssid; }

void netcfg_status(char *buf, int cap) {
    if (s_connected) snprintf(buf, cap, "CONNECTED  %s", s_ip);
    else if (s_connecting) snprintf(buf, cap, "CONNECTING...");
    else if (s_started) snprintf(buf, cap, "AP: %s", s_ap_ssid);
    else snprintf(buf, cap, "NOT STARTED");
}

/* ---------------- credentials (NVS namespace "wifi") ---------------- */
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

static void start_sta(const char *ssid, const char *pass) {
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    setstr(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
    setstr(sta.sta.password, sizeof(sta.sta.password), pass);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_connected = false;
    s_connecting = true;
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
            if (s_connecting) esp_wifi_connect();   /* keep retrying */
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_connecting = false;
        ESP_LOGI(TAG, "got IP %s", s_ip);
    }
}

/* ---------------- web page ---------------- */
static const char *PAGE =
    "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>RaceBrick WiFi</title></head><body style=\"font-family:sans-serif;margin:22px;background:#111;color:#eee\">"
    "<h2 style=\"color:#4cf\">RaceBrick WiFi Setup</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "SSID<br><input name=\"ssid\" autocomplete=\"off\" style=\"width:100%;padding:10px;font-size:16px\"><br><br>"
    "Password<br><input name=\"pass\" type=\"password\" style=\"width:100%;padding:10px;font-size:16px\"><br><br>"
    "<button style=\"padding:12px 22px;font-size:16px\">Connect</button>"
    "</form><p id=\"s\" style=\"color:#9cf\"></p>"
    "<script>setInterval(function(){fetch('/status').then(function(r){return r.text()}).then(function(t){"
    "document.getElementById('s').textContent=t})},1000)</script></body></html>";

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

    char ssid[33], pass[65];
    getfield(body, "ssid", ssid, sizeof ssid);
    getfield(body, "pass", pass, sizeof pass);
    if (!ssid[0]) { httpd_resp_send(req, "SSID required", HTTPD_RESP_USE_STRLEN); return ESP_OK; }

    save_creds(ssid, pass);
    start_sta(ssid, pass);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta name=viewport content=\"width=device-width,initial-scale=1\"></head>"
        "<body style=\"font-family:sans-serif;margin:22px;background:#111;color:#eee\">"
        "<h2 style=\"color:#4cf\">Saving...</h2><p>Connecting to your WiFi. You can close this page.</p>"
        "<p><a href=\"/\" style=\"color:#9cf\">back</a></p></body></html>",
        HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
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
    httpd_register_uri_handler(s_httpd, &u1);
    httpd_register_uri_handler(s_httpd, &u2);
    httpd_register_uri_handler(s_httpd, &u3);
    httpd_register_uri_handler(s_httpd, &u4);
    httpd_register_uri_handler(s_httpd, &u5);
    httpd_register_uri_handler(s_httpd, &u6);
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
    if (load_creds(ssid, sizeof ssid, pass, sizeof pass)) {
        wifi_config_t sta;
        memset(&sta, 0, sizeof(sta));
        setstr(sta.sta.ssid, sizeof(sta.sta.ssid), ssid);
        setstr(sta.sta.password, sizeof(sta.sta.password), pass);
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        s_connecting = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    start_httpd();
    s_started = true;
    ESP_LOGI(TAG, "SoftAP '%s' up, open http://192.168.4.1", s_ap_ssid);
}
