#include "tstore.h"
#include "extflash.h"
#include "rb_uuid.h"
#include "race_store.h"
#include <string.h>
#include <stddef.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
} ts_rec_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t N;
    uint8_t  vehicle, nsec, flags, hasBest;
    char     name[48];
    float    lengthM, widthM, lapTarget, start_hdg, line_hdg, best;
    double   glat0, glon0, line_lat, line_lon;
    float    nscale, nox, noy;
    float    sections[MAX_SECTIONS];
    pt_t     points[TRK_N];
    uint32_t hash;
} ts_rec_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t N;
    uint8_t  vehicle, nsec, flags, hasBest;
    char     name[48];
    float    lengthM, widthM, lapTarget, start_hdg, line_hdg, best, start_offset;
    double   glat0, glon0, line_lat, line_lon;
    float    nscale, nox, noy;
    float    sections[MAX_SECTIONS];
    pt_t     points[TRK_N];
    uint32_t hash;
} ts_rec_v3_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t N;
    uint8_t  vehicle, nsec, flags, hasBest;
    char     name[48];
    float    lengthM, widthM, lapTarget, start_hdg, line_hdg, best, start_offset, start_width;
    double   glat0, glon0, line_lat, line_lon;
    float    nscale, nox, noy;
    float    sections[MAX_SECTIONS];
    pt_t     points[TRK_N];
    uint32_t hash;
} ts_rec_v4_t;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t N;
    uint8_t  vehicle, nsec, flags, hasBest;
    char     name[48];
    float    lengthM, widthM, lapTarget, start_hdg, line_hdg, best, start_offset, start_width;
    double   glat0, glon0, line_lat, line_lon;
    float    nscale, nox, noy;
    float    sections[MAX_SECTIONS];
    pt_t     points[TRK_N];
    uint32_t hash;              /* v4 compatibility hash; unused for v5 */
    rb_uuid_t track_id;
    rb_uuid_t revision_id;
    rb_uuid_t parent_revision_id;
    uint32_t revision;
    uint32_t hash5;
} ts_rec_t;

typedef struct {
    rb_uuid_t id;
    uint16_t slot;
    uint32_t revision;
} track_head_t;

#define TSTORE_MAX_HEADS 32

static ts_rec_t s_rec;
static int s_count;
static int s_cap;
static bool s_ext;
static uint8_t s_used[(TSTORE_EXT_SLOTS + 7) / 8];
static track_head_t s_heads[TSTORE_MAX_HEADS];
static int s_nheads;
static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;

_Static_assert(sizeof(ts_rec_t) <= TSTORE_SLOT, "ts_rec_t exceeds a slot");
_Static_assert(offsetof(ts_rec_t, hash) == offsetof(ts_rec_v4_t, hash),
               "v5 must keep the v4 prefix layout");

static void tstore_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void tstore_unlock(void) {
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static size_t slot_off(int slot) { return (size_t)slot * TSTORE_SLOT; }

static bool rec_valid(void) {
    if (s_rec.magic != TSTORE_MAGIC) return false;
    if (s_rec.N != TRK_N) return false;
    if (s_rec.ver == 1) {
        const ts_rec_v1_t *v = (const ts_rec_v1_t *)&s_rec;
        uint32_t h = fnv((const uint8_t *)v, (int)offsetof(ts_rec_v1_t, hash));
        return h == v->hash;
    }
    if (s_rec.ver == 2) {
        const ts_rec_v2_t *v = (const ts_rec_v2_t *)&s_rec;
        uint32_t h = fnv((const uint8_t *)v, (int)offsetof(ts_rec_v2_t, hash));
        return h == v->hash;
    }
    if (s_rec.ver == 3) {
        const ts_rec_v3_t *v = (const ts_rec_v3_t *)&s_rec;
        uint32_t h = fnv((const uint8_t *)v, (int)offsetof(ts_rec_v3_t, hash));
        return h == v->hash;
    }
    if (s_rec.ver == 4) {
        const ts_rec_v4_t *v = (const ts_rec_v4_t *)&s_rec;
        uint32_t h = fnv((const uint8_t *)v, (int)offsetof(ts_rec_v4_t, hash));
        return h == v->hash;
    }
    if (s_rec.ver != 5) return false;
    uint32_t h5 = fnv((const uint8_t *)&s_rec, (int)offsetof(ts_rec_t, hash5));
    return h5 == s_rec.hash5;
}

static bool slot_used(int slot) {
    return slot >= 0 && slot < s_cap && (s_used[slot >> 3] & (1u << (slot & 7)));
}

static void set_used(int slot, bool used) {
    if (slot < 0 || slot >= s_cap) return;
    if (used) s_used[slot >> 3] |= (uint8_t)(1u << (slot & 7));
    else s_used[slot >> 3] &= (uint8_t)~(1u << (slot & 7));
}

static void head_add(const ts_rec_t *r, int slot) {
    if (!r || r->ver < 5 || rb_uuid_is_zero(&r->track_id) || r->revision == 0) return;
    for (int i = 0; i < s_nheads; i++) {
        if (rb_uuid_equal(&s_heads[i].id, &r->track_id)) {
            if (r->revision > s_heads[i].revision) {
                s_heads[i].slot = (uint16_t)slot;
                s_heads[i].revision = r->revision;
            }
            return;
        }
    }
    if (s_nheads >= TSTORE_MAX_HEADS) return;
    s_heads[s_nheads].id = r->track_id;
    s_heads[s_nheads].slot = (uint16_t)slot;
    s_heads[s_nheads].revision = r->revision;
    s_nheads++;
}

static void head_remove_slot(int slot) {
    for (int i = 0; i < s_nheads; i++) {
        if (s_heads[i].slot == (uint16_t)slot) {
            s_heads[i] = s_heads[s_nheads - 1];
            s_nheads--;
            return;
        }
    }
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

typedef struct {
    int slot;
    uint32_t revision;
} track_rev_ref_t;

static bool slot_referenced_by_session(int slot) {
    race_hdr_t h;
    for (int i = 0; i < RACE_SLOTS; i++) {
        if (race_store_info(i, &h) && h.track_slot == (uint16_t)slot) return true;
    }
    return false;
}

static void prune_track_revisions(const rb_uuid_t *track_id) {
    if (!track_id || rb_uuid_is_zero(track_id)) return;
    track_rev_ref_t *revs = (track_rev_ref_t *)heap_caps_malloc(
        sizeof(track_rev_ref_t) * TSTORE_EXT_SLOTS,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!revs) return;
    int n = 0;
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i) || !rec_valid()) continue;
        if (s_rec.ver < 5 || !rb_uuid_equal(&s_rec.track_id, track_id)) continue;
        if (n < TSTORE_EXT_SLOTS) {
            revs[n].slot = i;
            revs[n].revision = s_rec.revision;
            n++;
        }
    }

    /* Keep the current + 3 newest revisions. Also keep any revision still
     * referenced by a raw Session, so history pruning never breaks a Session. */
    for (int i = 0; i < n; i++) {
        if (revs[i].revision == 0) continue;
        int newer = 0;
        for (int j = 0; j < n; j++) {
            if (j != i && revs[j].revision > revs[i].revision) newer++;
        }
        bool referenced = slot_referenced_by_session(revs[i].slot);
        if (newer < 4 || referenced) continue;
        memset(&s_rec, 0xFF, sizeof(s_rec));
        if (write_slot(revs[i].slot)) {
            set_used(revs[i].slot, false);
            head_remove_slot(revs[i].slot);
            ESP_LOGI(TAG, "pruned track revision slot=%d rev=%u",
                     revs[i].slot, (unsigned)revs[i].revision);
        }
    }
    free(revs);
}

bool tstore_begin(void) {
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    tstore_lock();
    s_ext = ext_available();
    if (s_ext) {
        s_cap = TSTORE_EXT_SLOTS;
        s_part = NULL;
        ESP_LOGI(TAG, "using external W25Q128, %d track slots", s_cap);
    } else {
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
        if (!s_part) { ESP_LOGE(TAG, "no 'storage' data/spiffs partition"); tstore_unlock(); return false; }
        s_cap = TSTORE_INT_SLOTS;
        ESP_LOGW(TAG, "external flash unavailable, fallback internal %d slots", s_cap);
    }
    s_count = 0;
    s_nheads = 0;
    memset(s_used, 0, sizeof(s_used));
    int valid_slots = 0;
    for (int i = 0; i < s_cap; i++) {
        if (read_slot(i) && rec_valid()) {
            valid_slots++;
            set_used(i, true);
        }
    }

    /* One-time migration: give every pre-v5 record a stable track UUID and
     * an initial revision. The record stays in the same slot. */
    track_t *tmp = (track_t *)heap_caps_malloc(sizeof(track_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tmp) {
        ESP_LOGE(TAG, "track migration buffer allocation failed");
        tstore_unlock();
        return false;
    }
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i) || !rec_valid()) continue;
        if (s_rec.ver >= 5 && !rb_uuid_is_zero(&s_rec.track_id) &&
            s_rec.revision > 0 && !rb_uuid_is_zero(&s_rec.revision_id)) continue;
        if (!tstore_load(i, tmp)) continue;
        if (rb_uuid_is_zero(&tmp->id)) rb_uuid_new(&tmp->id);
        if (tmp->revision == 0) tmp->revision = 1;
        if (rb_uuid_is_zero(&tmp->rev_id)) rb_uuid_new(&tmp->rev_id);
        rb_uuid_clear(&tmp->parent_rev_id);
        if (!tstore_save(i, tmp)) {
            ESP_LOGE(TAG, "track UUID migration failed in slot %d", i);
        } else {
            ESP_LOGI(TAG, "migrated track slot %d to uuid/revision", i);
        }
    }
    free(tmp);

    /* Rebuild the head cache from all valid records. Archived revisions stay
     * on flash but only the highest revision per track is exposed. */
    s_nheads = 0;
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i) || !rec_valid()) continue;
        set_used(i, true);
        head_add(&s_rec, i);
    }
    s_count = s_nheads;
    ESP_LOGI(TAG, "%d tracks (%d valid slots)/%d used (%s)",
             s_count, valid_slots, s_cap, s_ext ? "external" : "internal");
    tstore_unlock();
    return true;
}

int tstore_count(void) { return s_count; }
int tstore_capacity(void) { return s_cap; }

int tstore_slots(int *out, int cap) {
    tstore_lock();
    int n = 0;
    for (int i = 0; i < s_nheads && n < cap; i++) {
        if (out) out[n++] = s_heads[i].slot;
    }
    tstore_unlock();
    return n;
}

int tstore_head_slots(int *out, int cap) {
    return tstore_slots(out, cap);
}

int tstore_first_free(void) {
    tstore_lock();
    if (!s_ext && !s_part) { tstore_unlock(); return -1; }
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i)) { tstore_unlock(); return i; }
        if (!rec_valid()) { tstore_unlock(); return i; }
    }
    tstore_unlock();
    return -1;
}

bool tstore_save(int slot, track_t *t) {
    tstore_lock();
    if ((!s_ext && !s_part) || slot < 0 || slot >= s_cap || !t) { tstore_unlock(); return false; }
    if (rb_uuid_is_zero(&t->id)) rb_uuid_new(&t->id);
    if (t->revision == 0) t->revision = 1;
    if (rb_uuid_is_zero(&t->rev_id)) rb_uuid_new(&t->rev_id);
    if (t->revision <= 1) rb_uuid_clear(&t->parent_rev_id);
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.magic = TSTORE_MAGIC;
    s_rec.ver = 5;
    s_rec.N = TRK_N;
    s_rec.vehicle = (uint8_t)t->vehicle;
    s_rec.nsec = (uint8_t)t->nsec;
    s_rec.flags = (t->geo ? 1 : 0) | (t->reverse ? 2 : 0);
    s_rec.hasBest = t->hasBest ? 1 : 0;
    snprintf(s_rec.name, sizeof(s_rec.name), "%s", t->name);
    s_rec.lengthM = t->lengthM;
    s_rec.widthM = t->widthM > 0 ? t->widthM : TRACK_WIDTH_M;
    s_rec.lapTarget = t->lapTarget;
    s_rec.start_hdg = t->start_hdg;
    s_rec.line_hdg = t->line_hdg;
    s_rec.best = t->best;
    s_rec.start_offset = t->start_offset;
    s_rec.start_width = t->start_width > 0 ? t->start_width : TRACK_WIDTH_M;
    s_rec.glat0 = t->glat0;
    s_rec.glon0 = t->glon0;
    s_rec.line_lat = t->line_lat;
    s_rec.line_lon = t->line_lon;
    s_rec.nscale = t->nscale;
    s_rec.nox = t->nox;
    s_rec.noy = t->noy;
    for (int i = 0; i < MAX_SECTIONS; i++) s_rec.sections[i] = t->sections[i];
    memcpy(s_rec.points, t->points, sizeof(pt_t) * TRK_N);
    s_rec.track_id = t->id;
    s_rec.revision_id = t->rev_id;
    s_rec.parent_revision_id = t->parent_rev_id;
    s_rec.revision = t->revision;
    s_rec.hash = 0;
    s_rec.hash5 = fnv((const uint8_t *)&s_rec, (int)offsetof(ts_rec_t, hash5));

    if (!write_slot(slot)) {
        ESP_LOGE(TAG, "write slot %d failed", slot);
        tstore_unlock();
        return false;
    }
    set_used(slot, true);
    /* A slot reused for another track must not leave a stale head entry. */
    for (int i = 0; i < s_nheads; i++) {
        if (s_heads[i].slot == (uint16_t)slot &&
            !rb_uuid_equal(&s_heads[i].id, &t->id)) {
            s_heads[i] = s_heads[s_nheads - 1];
            s_nheads--;
            i--;
        }
    }
    head_add(&s_rec, slot);
    s_count = s_nheads;
    t->store_slot = slot;
    char idbuf[40];
    rb_uuid_format(&t->id, idbuf, sizeof(idbuf));
    ESP_LOGI(TAG, "saved '%s' slot=%d rev=%u id=%s (%d bytes, %s)",
             t->name, slot, (unsigned)t->revision, idbuf,
             (int)sizeof(s_rec), s_ext ? "ext" : "int");
    tstore_unlock();
    return true;
}

bool tstore_commit(int *slot_io, track_t *t) {
    if (!slot_io || !t) return false;
    tstore_lock();
    track_t *old = (track_t *)heap_caps_malloc(sizeof(track_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!old) {
        tstore_unlock();
        return false;
    }
    int old_slot = *slot_io;
    if (!tstore_load(old_slot, old)) {
        free(old);
        tstore_unlock();
        return false;
    }
    if (rb_uuid_is_zero(&old->id) || old->revision == 0 ||
        rb_uuid_is_zero(&old->rev_id)) {
        free(old);
        tstore_unlock();
        return false;
    }
    t->id = old->id;
    t->parent_rev_id = old->rev_id;
    t->revision = old->revision + 1;
    rb_uuid_new(&t->rev_id);
    int slot = tstore_first_free();
    if (slot < 0) {
        free(old);
        tstore_unlock();
        return false;
    }
    t->store_slot = slot;
    bool ok = tstore_save(slot, t);
    free(old);
    if (!ok) {
        tstore_unlock();
        return false;
    }
    prune_track_revisions(&t->id);
    s_count = s_nheads;
    *slot_io = slot;
    tstore_unlock();
    return true;
}

bool tstore_load(int slot, track_t *t) {
    tstore_lock();
    if ((!s_ext && !s_part) || slot < 0 || slot >= s_cap) { tstore_unlock(); return false; }
    if (!read_slot(slot) || !rec_valid()) { tstore_unlock(); return false; }

    memset(t, 0, sizeof(*t));
    t->N = TRK_N;
    if (s_rec.ver == 1) {
        ts_rec_v1_t *v = (ts_rec_v1_t *)&s_rec;
        t->lengthM = v->lengthM;
        t->widthM = TRACK_WIDTH_M;
        t->lapTarget = v->lapTarget;
        t->vehicle = v->vehicle;
        t->nsec = v->nsec;
        t->geo = (v->flags & 1) != 0;
        t->reverse = (v->flags & 2) != 0;
        t->hasBest = v->hasBest != 0;
        snprintf(t->name, sizeof(t->name), "%.*s", (int)sizeof(v->name), v->name);
        t->start_hdg = v->start_hdg;
        t->line_hdg = v->line_hdg;
        t->best = v->best;
        t->glat0 = v->glat0;
        t->glon0 = v->glon0;
        t->line_lat = v->line_lat;
        t->line_lon = v->line_lon;
        t->nscale = v->nscale;
        t->nox = v->nox;
        t->noy = v->noy;
        t->start_width = TRACK_WIDTH_M;
        for (int i = 0; i < MAX_SECTIONS; i++) t->sections[i] = v->sections[i];
        memcpy(t->points, v->points, sizeof(pt_t) * TRK_N);
    } else if (s_rec.ver == 2) {
        const ts_rec_v2_t *v = (const ts_rec_v2_t *)&s_rec;
        t->lengthM = v->lengthM;
        t->widthM = v->widthM > 0 ? v->widthM : TRACK_WIDTH_M;
        t->lapTarget = v->lapTarget;
        t->vehicle = v->vehicle;
        t->nsec = v->nsec;
        t->geo = (v->flags & 1) != 0;
        t->reverse = (v->flags & 2) != 0;
        t->hasBest = v->hasBest != 0;
        memcpy(t->name, v->name, sizeof(t->name));
        t->name[sizeof(t->name) - 1] = 0;
        t->start_hdg = v->start_hdg;
        t->line_hdg = v->line_hdg;
        t->best = v->best;
        t->glat0 = v->glat0;
        t->glon0 = v->glon0;
        t->line_lat = v->line_lat;
        t->line_lon = v->line_lon;
        t->nscale = v->nscale;
        t->nox = v->nox;
        t->noy = v->noy;
        t->start_width = v->widthM > 0 ? v->widthM : TRACK_WIDTH_M;
        for (int i = 0; i < MAX_SECTIONS; i++) t->sections[i] = v->sections[i];
        memcpy(t->points, v->points, sizeof(pt_t) * TRK_N);
    } else if (s_rec.ver == 3) {
        const ts_rec_v3_t *v = (const ts_rec_v3_t *)&s_rec;
        t->lengthM = v->lengthM;
        t->widthM = v->widthM > 0 ? v->widthM : TRACK_WIDTH_M;
        t->lapTarget = v->lapTarget;
        t->vehicle = v->vehicle;
        t->nsec = v->nsec;
        t->geo = (v->flags & 1) != 0;
        t->reverse = (v->flags & 2) != 0;
        t->hasBest = v->hasBest != 0;
        memcpy(t->name, v->name, sizeof(t->name));
        t->name[sizeof(t->name) - 1] = 0;
        t->start_hdg = v->start_hdg;
        t->line_hdg = v->line_hdg;
        t->best = v->best;
        t->start_offset = v->start_offset;
        t->start_width = v->widthM > 0 ? v->widthM : TRACK_WIDTH_M;
        t->glat0 = v->glat0;
        t->glon0 = v->glon0;
        t->line_lat = v->line_lat;
        t->line_lon = v->line_lon;
        t->nscale = v->nscale;
        t->nox = v->nox;
        t->noy = v->noy;
        for (int i = 0; i < MAX_SECTIONS; i++) t->sections[i] = v->sections[i];
        memcpy(t->points, v->points, sizeof(pt_t) * TRK_N);
    } else {
        t->lengthM = s_rec.lengthM;
        t->widthM = s_rec.widthM > 0 ? s_rec.widthM : TRACK_WIDTH_M;
        t->lapTarget = s_rec.lapTarget;
        t->vehicle = s_rec.vehicle;
        t->nsec = s_rec.nsec;
        t->geo = (s_rec.flags & 1) != 0;
        t->reverse = (s_rec.flags & 2) != 0;
        t->hasBest = s_rec.hasBest != 0;
        memcpy(t->name, s_rec.name, sizeof(t->name));
        t->name[sizeof(t->name) - 1] = 0;
        t->start_hdg = s_rec.start_hdg;
        t->line_hdg = s_rec.line_hdg;
        t->best = s_rec.best;
        t->start_offset = s_rec.start_offset;
        t->start_width = s_rec.start_width > 0 ? s_rec.start_width : TRACK_WIDTH_M;
        t->glat0 = s_rec.glat0;
        t->glon0 = s_rec.glon0;
        t->line_lat = s_rec.line_lat;
        t->line_lon = s_rec.line_lon;
        t->nscale = s_rec.nscale;
        t->nox = s_rec.nox;
        t->noy = s_rec.noy;
        for (int i = 0; i < MAX_SECTIONS; i++) t->sections[i] = s_rec.sections[i];
        memcpy(t->points, s_rec.points, sizeof(pt_t) * TRK_N);
    }
    if (s_rec.ver >= 5) {
        t->id = s_rec.track_id;
        t->rev_id = s_rec.revision_id;
        t->parent_rev_id = s_rec.parent_revision_id;
        t->revision = s_rec.revision;
    } else {
        rb_uuid_clear(&t->id);
        rb_uuid_clear(&t->rev_id);
        rb_uuid_clear(&t->parent_rev_id);
        t->revision = 0;
    }
    track_finalize(t);
    tstore_unlock();
    return true;
}

bool tstore_delete(int slot) {
    tstore_lock();
    if (slot < 0 || slot >= s_cap || !read_slot(slot) || !rec_valid()) {
        tstore_unlock();
        return false;
    }
    rb_uuid_t id = s_rec.track_id;
    bool have_id = s_rec.ver >= 5 && !rb_uuid_is_zero(&id);
    int erased = 0;
    for (int i = 0; i < s_cap; i++) {
        if (!read_slot(i) || !rec_valid()) continue;
        bool match = (i == slot);
        if (have_id && s_rec.ver >= 5 && rb_uuid_equal(&s_rec.track_id, &id)) match = true;
        if (!match) continue;
        memset(&s_rec, 0xFF, sizeof(s_rec));
        if (write_slot(i)) {
            set_used(i, false);
            head_remove_slot(i);
            erased++;
        }
    }
    s_count = s_nheads;
    tstore_unlock();
    return erased > 0;
}
