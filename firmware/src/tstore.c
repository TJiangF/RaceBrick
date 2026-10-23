#include "tstore.h"
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "tstore";

#define TSTORE_MAGIC 0x52425452u   /* 'RBTR' */
#define TSTORE_SLOT  4096u

/* Stored in the internal-flash "storage" partition (data/spiffs subtype).
 * The external W25Q128 cannot be used together with octal PSRAM (GPIO33-37). */
typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t N;
    uint8_t  vehicle, nsec, flags, hasBest;
    char     name[8];
    float    lengthM, lapTarget, start_hdg, line_hdg, best;
    double   glat0, glon0, line_lat, line_lon;
    float    nscale, nox, noy;
    float    sections[MAX_SECTIONS];
    pt_t     points[TRK_N];
    uint32_t hash;
} ts_rec_t;

static ts_rec_t s_rec;
static int s_count;
static const esp_partition_t *s_part;

_Static_assert(sizeof(ts_rec_t) <= TSTORE_SLOT, "ts_rec_t exceeds a slot");

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static size_t slot_off(int slot) { return (size_t)slot * TSTORE_SLOT; }

static bool rec_valid(void) {
    if (s_rec.magic != TSTORE_MAGIC) return false;
    if (s_rec.ver != 1 || s_rec.N != TRK_N) return false;
    uint32_t h = fnv((const uint8_t *)&s_rec, (int)offsetof(ts_rec_t, hash));
    return h == s_rec.hash;
}

bool tstore_begin(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (!s_part) { ESP_LOGE(TAG, "no 'storage' data/spiffs partition"); return false; }
    s_count = 0;
    for (int i = 0; i < TSTORE_SLOTS; i++) {
        if (esp_partition_read(s_part, slot_off(i), &s_rec, sizeof(s_rec)) != ESP_OK) continue;
        if (rec_valid()) s_count++;
    }
    ESP_LOGI(TAG, "%d/%d slots used (internal flash)", s_count, TSTORE_SLOTS);
    return true;
}

int tstore_count(void) { return s_count; }
int tstore_capacity(void) { return TSTORE_SLOTS; }

int tstore_first_free(void) {
    if (!s_part) return -1;
    for (int i = 0; i < TSTORE_SLOTS; i++) {
        if (esp_partition_read(s_part, slot_off(i), &s_rec, sizeof(s_rec)) != ESP_OK) return i;
        if (!rec_valid()) return i;
    }
    return -1;
}

bool tstore_save(int slot, const track_t *t) {
    if (!s_part || slot < 0 || slot >= TSTORE_SLOTS) return false;
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.magic = TSTORE_MAGIC;
    s_rec.ver = 1;
    s_rec.N = TRK_N;
    s_rec.vehicle = (uint8_t)t->vehicle;
    s_rec.nsec = (uint8_t)t->nsec;
    s_rec.flags = t->geo ? 1 : 0;
    s_rec.hasBest = t->hasBest ? 1 : 0;
    memcpy(s_rec.name, t->name, sizeof(s_rec.name));
    s_rec.lengthM = t->lengthM;
    s_rec.lapTarget = t->lapTarget;
    s_rec.start_hdg = t->start_hdg;
    s_rec.line_hdg = t->line_hdg;
    s_rec.best = t->best;
    s_rec.glat0 = t->glat0;
    s_rec.glon0 = t->glon0;
    s_rec.line_lat = t->line_lat;
    s_rec.line_lon = t->line_lon;
    s_rec.nscale = t->nscale;
    s_rec.nox = t->nox;
    s_rec.noy = t->noy;
    for (int i = 0; i < MAX_SECTIONS; i++) s_rec.sections[i] = t->sections[i];
    memcpy(s_rec.points, t->points, sizeof(pt_t) * TRK_N);
    s_rec.hash = fnv((const uint8_t *)&s_rec, (int)offsetof(ts_rec_t, hash));

    size_t off = slot_off(slot);
    if (esp_partition_erase_range(s_part, off, TSTORE_SLOT) != ESP_OK) {
        ESP_LOGE(TAG, "erase %d failed", slot); return false;
    }
    if (esp_partition_write(s_part, off, &s_rec, sizeof(s_rec)) != ESP_OK) {
        ESP_LOGE(TAG, "write %d failed", slot); return false;
    }
    if (s_count < TSTORE_SLOTS) s_count++;
    ESP_LOGI(TAG, "saved '%s' to slot %d (%d bytes)", t->name, slot, (int)sizeof(s_rec));
    return true;
}

bool tstore_load(int slot, track_t *t) {
    if (!s_part || slot < 0 || slot >= TSTORE_SLOTS) return false;
    if (esp_partition_read(s_part, slot_off(slot), &s_rec, sizeof(s_rec)) != ESP_OK) return false;
    if (!rec_valid()) return false;

    memset(t, 0, sizeof(*t));
    t->N = TRK_N;
    t->lengthM = s_rec.lengthM;
    t->lapTarget = s_rec.lapTarget;
    t->vehicle = s_rec.vehicle;
    t->nsec = s_rec.nsec;
    t->geo = (s_rec.flags & 1) != 0;
    t->hasBest = s_rec.hasBest != 0;
    memcpy(t->name, s_rec.name, sizeof(t->name));
    t->name[sizeof(t->name) - 1] = 0;
    t->start_hdg = s_rec.start_hdg;
    t->line_hdg = s_rec.line_hdg;
    t->best = s_rec.best;
    t->glat0 = s_rec.glat0;
    t->glon0 = s_rec.glon0;
    t->line_lat = s_rec.line_lat;
    t->line_lon = s_rec.line_lon;
    t->nscale = s_rec.nscale;
    t->nox = s_rec.nox;
    t->noy = s_rec.noy;
    for (int i = 0; i < MAX_SECTIONS; i++) t->sections[i] = s_rec.sections[i];
    memcpy(t->points, s_rec.points, sizeof(pt_t) * TRK_N);
    track_finalize(t);
    return true;
}
