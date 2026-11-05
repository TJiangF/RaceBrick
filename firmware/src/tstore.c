#include "tstore.h"
#include "extflash.h"
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "tstore";

#define TSTORE_MAGIC 0x52425452u   /* 'RBTR' */
#define TSTORE_SLOT  4096u

/* Track geometry is stored directly in the external W25Q128 from offset 0.
 * Slot 0 can also be used for the boot sample track. The internal storage
 * partition remains a fallback when the external flash is unavailable. */
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
static int s_cap;
static bool s_ext;
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

static bool ext_available(void) {
    uint8_t id[3] = {0};
    if (!ext_flash_probe(id)) return false;
    return id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18;
}

static bool read_slot(int slot) {
    if (slot < 0 || slot >= s_cap) return false;
    if (s_ext) return ext_flash_read((uint32_t)slot_off(slot), &s_rec, sizeof(s_rec));
    return esp_partition_read(s_part, slot_off(slot), &s_rec, sizeof(s_rec)) == ESP_OK;
}

static bool write_slot(int slot) {
    if (slot < 0 || slot >= s_cap) return false;
    if (s_ext) {
        if (!ext_flash_erase_sector((uint32_t)slot_off(slot))) return false;
        return ext_flash_write((uint32_t)slot_off(slot), &s_rec, sizeof(s_rec));
    }
    if (esp_partition_erase_range(s_part, slot_off(slot), TSTORE_SLOT) != ESP_OK) return false;
    return esp_partition_write(s_part, slot_off(slot), &s_rec, sizeof(s_rec)) == ESP_OK;
}

bool tstore_begin(void) {
    s_ext = ext_available();
    if (s_ext) {
        s_cap = TSTORE_EXT_SLOTS;
        s_part = NULL;
        ESP_LOGI(TAG, "using external W25Q128, %d track slots", s_cap);
    } else {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (!s_part) { ESP_LOGE(TAG, "no 'storage' data/spiffs partition"); return false; }
        s_cap = TSTORE_INT_SLOTS;
        ESP_LOGW(TAG, "external flash unavailable, fallback internal %d slots", s_cap);
    }
    s_count = 0;
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i)) continue;
        if (rec_valid()) s_count++;
    }
    ESP_LOGI(TAG, "%d/%d track slots used (%s)", s_count, s_cap, s_ext ? "external" : "internal");
    return true;
}

int tstore_count(void) { return s_count; }
int tstore_capacity(void) { return s_cap; }

int tstore_first_free(void) {
    if (!s_ext && !s_part) return -1;
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i)) return i;
        if (!rec_valid()) return i;
    }
    return -1;
}

bool tstore_save(int slot, const track_t *t) {
    if ((!s_ext && !s_part) || slot < 0 || slot >= s_cap) return false;
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

    if (!write_slot(slot)) {
        ESP_LOGE(TAG, "write slot %d failed", slot);
        return false;
    }
    if (s_count < s_cap) s_count++;
    ESP_LOGI(TAG, "saved '%s' to slot %d (%d bytes, %s)", t->name, slot, (int)sizeof(s_rec), s_ext ? "ext" : "int");
    return true;
}

bool tstore_load(int slot, track_t *t) {
    if ((!s_ext && !s_part) || slot < 0 || slot >= s_cap) return false;
    if (!read_slot(slot) || !rec_valid()) return false;

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

bool tstore_delete(int slot) {
    if (slot < 0 || slot >= s_cap) return false;
    memset(&s_rec, 0xFF, sizeof(s_rec));
    bool ok = write_slot(slot);
    if (ok && s_count > 0) s_count--;
    return ok;
}
