#include "track_img.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "trackimg";

#define SAT_MAGIC       0x54415452u   /* 'RTAT' */
#define SAT_VER         1
#define SAT_SLOTS       16
#define SAT_SLOT_SIZE   (2u * 1024u * 1024u / SAT_SLOTS)
#define SAT_HDR_BYTES   256u
#define SAT_MAX_W       320
#define SAT_MAX_H       320
#define SAT_MAX_PIXELS  (SAT_MAX_W * SAT_MAX_H)

typedef struct {
    uint32_t magic;
    uint16_t ver;
    int16_t  track_slot;
    uint16_t w, h;
    uint32_t len;
    float    x0, y0, x1, y1;
    uint32_t hash;
} sat_hdr_t;

_Static_assert(sizeof(sat_hdr_t) <= SAT_HDR_BYTES, "satellite header too large");

static const esp_partition_t *s_part;
static bool s_part_tried;

static uint16_t *s_img;
static int s_img_slot = -2;
static int s_img_w, s_img_h;
static float s_x0, s_y0, s_x1, s_y1;
static uint8_t *s_pal;

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static const esp_partition_t *part(void) {
    if (!s_part_tried) {
        s_part_tried = true;
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                          "storage");
        if (s_part && s_part->size < SAT_SLOTS * SAT_SLOT_SIZE) s_part = NULL;
        if (s_part) ESP_LOGI(TAG, "satellite store: %u slots x %u KiB",
                             SAT_SLOTS, SAT_SLOT_SIZE / 1024);
    }
    return s_part;
}

static uint32_t slot_off(int slot) {
    return (uint32_t)slot * SAT_SLOT_SIZE;
}

static bool hdr_ok(const sat_hdr_t *h) {
    return h->magic == SAT_MAGIC && h->ver == SAT_VER &&
           h->track_slot >= 0 && h->w > 0 && h->h > 0 &&
           h->w <= SAT_MAX_W && h->h <= SAT_MAX_H &&
           h->len > 6 && h->len <= SAT_SLOT_SIZE - SAT_HDR_BYTES &&
           isfinite(h->x0) && isfinite(h->y0) &&
           isfinite(h->x1) && isfinite(h->y1) &&
           h->x1 > h->x0 && h->y1 > h->y0;
}

static bool read_hdr(int slot, sat_hdr_t *h) {
    const esp_partition_t *p = part();
    if (!p) return false;
    return esp_partition_read(p, slot_off(slot), h, sizeof(*h)) == ESP_OK && hdr_ok(h);
}

static int find_slot(int track_slot, bool create) {
    int empty = -1;
    for (int i = 0; i < SAT_SLOTS; i++) {
        sat_hdr_t h;
        if (!read_hdr(i, &h)) {
            if (empty < 0) empty = i;
            continue;
        }
        if (h.track_slot == track_slot) return i;
    }
    return create ? empty : -1;
}

static bool decode_pal16(const uint8_t *b, int len, uint16_t *out, int *ow, int *oh) {
    if (len < 6) return false;
    int w = b[0] | (b[1] << 8);
    int h = b[2] | (b[3] << 8);
    int pc = b[4] | (b[5] << 8);
    if (w <= 0 || h <= 0 || w > SAT_MAX_W || h > SAT_MAX_H ||
        w * h > SAT_MAX_PIXELS || pc <= 0 || pc > 16) return false;
    const uint8_t *p = b + 6, *end = b + len;
    if (p + pc * 2 > end) return false;
    uint16_t pal[16];
    for (int i = 0; i < pc; i++) {
        pal[i] = (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
    }
    int n = w * h, k = 0;
    while (p < end && k < n) {
        uint8_t v = *p++;
        int run = (v >> 4) + 1;
        int col = v & 0x0F;
        if ((v >> 4) == 15) {
            if (p >= end) return false;
            run = 16 + (*p++);
        }
        uint16_t c = col < pc ? pal[col] : 0;
        if (k + run > n) run = n - k;
        for (int i = 0; i < run; i++) out[k++] = c;
    }
    if (k != n) return false;
    *ow = w;
    *oh = h;
    return true;
}

void track_img_invalidate(void) {
    s_img_slot = -2;
}

bool track_img_save(int track_slot, const uint8_t *pal16, int len,
                    float x0, float y0, float x1, float y1) {
    const esp_partition_t *p = part();
    if (!p || !pal16 || len < 6 || track_slot < 0 ||
        len > (int)(SAT_SLOT_SIZE - SAT_HDR_BYTES) ||
        !isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1) ||
        x1 <= x0 || y1 <= y0) return false;
    if (len < 6) return false;
    int w = pal16[0] | (pal16[1] << 8);
    int h = pal16[2] | (pal16[3] << 8);
    int pc = pal16[4] | (pal16[5] << 8);
    if (w <= 0 || h <= 0 || w > SAT_MAX_W || h > SAT_MAX_H ||
        w * h > SAT_MAX_PIXELS || pc <= 0 || pc > 16) return false;
    int slot = find_slot(track_slot, true);
    if (slot < 0) {
        ESP_LOGW(TAG, "satellite store full");
        return false;
    }
    sat_hdr_t hd;
    memset(&hd, 0, sizeof(hd));
    hd.magic = SAT_MAGIC;
    hd.ver = SAT_VER;
    hd.track_slot = (int16_t)track_slot;
    hd.w = (uint16_t)w;
    hd.h = (uint16_t)h;
    hd.len = (uint32_t)len;
    hd.x0 = x0; hd.y0 = y0; hd.x1 = x1; hd.y1 = y1;
    hd.hash = fnv(pal16, len);
    esp_err_t e = esp_partition_erase_range(p, slot_off(slot), SAT_SLOT_SIZE);
    if (e == ESP_OK)
        e = esp_partition_write(p, slot_off(slot), &hd, sizeof(hd));
    if (e == ESP_OK)
        e = esp_partition_write(p, slot_off(slot) + SAT_HDR_BYTES, pal16, len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "save slot=%d failed: %s", slot, esp_err_to_name(e));
        return false;
    }
    if (s_img_slot == track_slot) track_img_invalidate();
    ESP_LOGI(TAG, "saved track=%d slot=%d %dx%d len=%d bounds %.3f %.3f %.3f %.3f",
             track_slot, slot, w, h, len, (double)x0, (double)y0,
             (double)x1, (double)y1);
    return true;
}

bool track_img_get(int track_slot, const uint16_t **pixels, int *w, int *h,
                   float *x0, float *y0, float *x1, float *y1) {
    if (!pixels || track_slot < 0) return false;
    if (s_img_slot == track_slot) {
        *pixels = s_img; *w = s_img_w; *h = s_img_h;
        *x0 = s_x0; *y0 = s_y0; *x1 = s_x1; *y1 = s_y1;
        return true;
    }
    s_img_slot = -2;
    int slot = find_slot(track_slot, false);
    if (slot < 0) return false;
    sat_hdr_t hd;
    if (!read_hdr(slot, &hd)) return false;
    const esp_partition_t *p = part();
    if (!p) return false;
    if (!s_img) {
        s_img = heap_caps_malloc(SAT_MAX_PIXELS * sizeof(uint16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_pal = heap_caps_malloc(SAT_SLOT_SIZE - SAT_HDR_BYTES,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_img || !s_pal) return false;
    if (esp_partition_read(p, slot_off(slot) + SAT_HDR_BYTES, s_pal, hd.len) != ESP_OK)
        return false;
    if (fnv(s_pal, hd.len) != hd.hash) {
        ESP_LOGW(TAG, "hash mismatch track=%d", track_slot);
        return false;
    }
    int dw = 0, dh = 0;
    if (!decode_pal16(s_pal, hd.len, s_img, &dw, &dh)) return false;
    s_img_w = dw; s_img_h = dh;
    s_x0 = hd.x0; s_y0 = hd.y0; s_x1 = hd.x1; s_y1 = hd.y1;
    s_img_slot = track_slot;
    *pixels = s_img; *w = s_img_w; *h = s_img_h;
    *x0 = s_x0; *y0 = s_y0; *x1 = s_x1; *y1 = s_y1;
    return true;
}
