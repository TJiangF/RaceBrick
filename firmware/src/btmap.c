#include "btmap.h"
#include "config.h"
#include "gps.h"

#include <string.h>
#include <math.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/ble_hs_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"

void ble_store_config_init(void);

static const char *TAG = "btmap";

#define BT_W 320
#define BT_H 240
#define BT_FRAME_MAX (80 * 1024)
#define BT_MAX_PARTS 4000
#define BT_TYPE_FRAME 5
#define BT_DEVICE_NAME "RaceBrick"

/* 6e400001-b5a3-f393-e0a9-e50e24dcca9e */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
/* 6e400002-... (phone -> device) */
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
/* 6e400003-... (device -> phone) */
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static bool s_started, s_enabled, s_synced;
static bool s_connected, s_subscribed;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint8_t s_own_addr_type;

static uint16_t *s_front, *s_back;
static uint8_t *s_wire;
static SemaphoreHandle_t s_lock;
static volatile bool s_have;
static volatile int s_w, s_h;
static volatile int s_frames, s_last_bytes;
static volatile int64_t s_last_us;

static volatile bool s_ready_pending;
static volatile bool s_k_pending;
static volatile int s_k_id;
static int64_t s_last_gps_us;

static uint8_t s_write_buf[512];
static uint8_t s_seen[(BT_MAX_PARTS + 7) / 8];
static int s_rx_id = -1, s_rx_total, s_rx_parts, s_rx_got;

static int gap_event(struct ble_gap_event *event, void *arg);

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void advertise(void) {
    if (!s_enabled || !s_synced || s_connected) return;

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof params);
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) ESP_LOGI(TAG, "advertising as %s", BT_DEVICE_NAME);
    else if (rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start: %d", rc);
}

static void send_line(const char *text) {
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE || s_tx_handle == 0) return;
    uint16_t len = (uint16_t)strlen(text);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(text, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
    if (rc != 0) ESP_LOGD(TAG, "notify %d", rc);
}

static void send_gps(void) {
    gps_state_t *gp = gps_get();
    if (!(gp->fix_type >= 1 && gp->has_pos)) return;
    float acc = gp->hdop * 5.0f;
    if (!isfinite(acc) || acc <= 0 || acc > 99.9f) acc = 99.9f;
    char b[160];
    snprintf(b, sizeof b, "#G,%.7f,%.7f,%.1f,%.2f,%.1f,%.1f,0",
             gp->lat, gp->lon, gp->course, gp->speed_kmh / 3.6f, acc, gp->alt);
    send_line(b);
}

static bool decode_frame(const uint8_t *d, int n) {
    if (n < 48 || !s_back) return false;
    const uint8_t *p = d;
    uint16_t frame_id = rd16(p); p += 2;
    p += 4 + 4;                              /* x0, y0 */
    uint16_t w = rd16(p); p += 2;
    uint16_t h = rd16(p); p += 2;
    uint16_t flags = rd16(p); p += 2;
    if (w != BT_W || h != BT_H || !(flags & 0x8000)) {
        ESP_LOGW(TAG, "bad frame hdr %ux%u flags=%04x", w, h, flags);
        return false;
    }
    uint16_t pal[16];
    for (int i = 0; i < 16; i++) { pal[i] = rd16(p); p += 2; }

    const uint8_t *end = d + n;
    int total = BT_W * BT_H, idx = 0;
    while (idx < total && p < end) {
        uint8_t v = *p++;
        int run = (v >> 4) + 1;
        int col = v & 15;
        if ((v >> 4) == 15) {
            if (p >= end) break;
            run = 16 + *p++;
        }
        if (run > total - idx) run = total - idx;
        uint16_t c = pal[col];
        for (int k = 0; k < run; k++) s_back[idx++] = c;
    }
    if (idx != total) {
        ESP_LOGW(TAG, "short frame %d/%d id=%u", idx, total, frame_id);
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t *t = s_front; s_front = s_back; s_back = t;
    s_w = BT_W; s_h = BT_H; s_have = true; s_frames++;
    xSemaphoreGive(s_lock);
    s_last_bytes = n;
    s_last_us = esp_timer_get_time();
    s_k_id = frame_id;
    s_k_pending = true;
    if (s_frames == 1 || (s_frames % 10) == 0)
        ESP_LOGI(TAG, "frame %d: %dx%d, %d bytes", s_frames, BT_W, BT_H, n);
    return true;
}

static void reset_rx(void) {
    s_rx_id = -1;
    s_rx_total = s_rx_parts = s_rx_got = 0;
}

static void handle_bulk(const uint8_t *b, int len) {
    if (len < 15) return;
    int type = b[1];
    int id = b[2];
    int parts = rd16(b + 3);
    int seq = rd16(b + 5);
    uint32_t total = rd32(b + 7);
    uint32_t off = rd32(b + 11);
    int plen = len - 15;

    if (type != BT_TYPE_FRAME) return;       /* raster/route are not used by PHONE page */
    if (parts <= 0 || parts > BT_MAX_PARTS || seq >= parts || plen <= 0 ||
        total == 0 || total > BT_FRAME_MAX || off > total ||
        (uint32_t)plen > total - off) {
        reset_rx();
        return;
    }
    if (id != s_rx_id || total != s_rx_total || parts != s_rx_parts) {
        reset_rx();
        s_rx_id = id;
        s_rx_total = (int)total;
        s_rx_parts = parts;
        memset(s_seen, 0, sizeof s_seen);
    }
    if (!(s_seen[seq >> 3] & (1u << (seq & 7)))) {
        s_seen[seq >> 3] |= (uint8_t)(1u << (seq & 7));
        s_rx_got++;
    }
    memcpy(s_wire + off, b + 15, plen);
    if (s_rx_got == s_rx_parts) {
        decode_frame(s_wire, (int)total);
        reset_rx();
    }
}

static void handle_write(const uint8_t *b, int len) {
    if (len <= 0) return;
    if (len >= 15 && b[0] == 'M') {
        handle_bulk(b, len);
        return;
    }
    if (len >= 4 && memcmp(b, "#GB?", 4) == 0) s_ready_pending = true;
    /* #t/#m/#n and route/HUD text are ignored: the phone renders the whole frame. */
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0) return 0;
        if (len > sizeof s_write_buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (os_mbuf_copydata(ctxt->om, 0, len, s_write_buf) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        handle_write(s_write_buf, len);
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_rx_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_tx_uuid.u,
        .access_cb = gatt_access,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 }
};

static void find_tx(void) {
    uint16_t val = 0;
    if (ble_gatts_find_chr(&s_svc_uuid.u, &s_tx_uuid.u, NULL, &val) == 0)
        s_tx_handle = val;
    if (s_tx_handle) ESP_LOGI(TAG, "tx handle %u", s_tx_handle);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_connected = true;
            s_subscribed = false;
            s_last_gps_us = 0;
            reset_rx();
            ESP_LOGI(TAG, "connected (handle %u)", s_conn);
        } else {
            s_connected = false;
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_subscribed = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready_pending = false;
        s_k_pending = false;
        reset_rx();
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            if (s_subscribed) {
                s_ready_pending = true;
                s_last_gps_us = 0;
                ESP_LOGI(TAG, "notifications on");
            }
        }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", event->enc_change.status);
        if (event->enc_change.status == 0 && s_subscribed) s_ready_pending = true;
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static void on_sync(void) {
    s_synced = true;
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    if (!s_tx_handle) find_tx();
    advertise();
}

static void on_reset(int reason) {
    s_synced = false;
    s_connected = false;
    s_subscribed = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGW(TAG, "host reset %d", reason);
}

static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void btmap_start(void) {
    if (s_started) return;
    if (!s_enabled) return;

    s_front = heap_caps_malloc((size_t)BT_W * BT_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_back = heap_caps_malloc((size_t)BT_W * BT_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wire = heap_caps_malloc(BT_FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (!s_front || !s_back || !s_wire || !s_lock) {
        ESP_LOGE(TAG, "alloc failed");
        return;
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble init %d", rc);
        return;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BT_DEVICE_NAME);

    rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatt add %d", rc);
        return;
    }
    find_tx();
    ble_store_config_init();
    ble_att_set_preferred_mtu(517);
    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE up, waiting for phone");
}

void btmap_set_enabled(bool on) {
    if (on == s_enabled) return;
    s_enabled = on;
    if (on) {
        if (!s_started) btmap_start();
        else advertise();
    } else {
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, 0x13);
        ble_gap_adv_stop();
        ESP_LOGI(TAG, "BLE disabled");
    }
}

bool btmap_enabled(void) { return s_enabled; }
bool btmap_connected(void) { return s_connected; }
bool btmap_subscribed(void) { return s_subscribed; }
int btmap_frames(void) { return s_frames; }
int btmap_last_bytes(void) { return s_last_bytes; }
bool btmap_has_frame(void) { return s_have; }
int btmap_age_ms(void) { return s_last_us ? (int)((esp_timer_get_time() - s_last_us) / 1000) : -1; }

const char *btmap_state(void) {
    if (!s_enabled) return "BT OFF";
    if (!s_started) return "BT START";
    if (!s_connected) return "ADVERTISING";
    if (!s_subscribed) return "CONNECTED";
    if (s_have) return "STREAMING";
    return "READY";
}

const uint16_t *btmap_frame_lock(int *w, int *h) {
    if (!s_lock) { if (w) *w = 0; if (h) *h = 0; return NULL; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (w) *w = s_w;
    if (h) *h = s_h;
    return s_have ? s_front : NULL;
}
void btmap_frame_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

void btmap_poll(void) {
    if (!s_started || !s_enabled) return;
    if (s_ready_pending && s_subscribed) {
        s_ready_pending = false;
        send_line("U,1");
        ESP_LOGI(TAG, "phone verified");
    }
    if (s_k_pending && s_subscribed) {
        s_k_pending = false;
        char b[20];
        snprintf(b, sizeof b, "K,5,%d", s_k_id);
        send_line(b);
    }
    int64_t now = esp_timer_get_time();
    if (s_subscribed && now - s_last_gps_us >= 1000000) {
        s_last_gps_us = now;
        send_gps();
    }
}
