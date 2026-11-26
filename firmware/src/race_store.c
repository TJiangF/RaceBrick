#include "race_store.h"
#include "extflash.h"
#include "tstore.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *TAG = "racestore";
#define RACE_MAGIC 0x52534331u  /* 'RSC1' */

static bool s_active;
static int s_slot = -1;
static race_hdr_t s_hdr;
static uint32_t s_data;
static uint32_t s_last_sector = 0xFFFFFFFFu;
static uint64_t s_start_us;
static uint64_t s_last_fix_time_us;
static uint32_t s_last_fix_seq;
static bool s_start_valid;
static uint32_t s_lap_first[RACE_LAPS];
static uint32_t s_lap_last[RACE_LAPS];
static int s_hdr_cache_slot = -1;
static race_hdr_t s_hdr_cache;

_Static_assert(sizeof(race_hdr_t) <= RACE_HDR_SIZE, "race header too large");
_Static_assert(sizeof(race_sample_t) == RACE_SAMPLE_SIZE, "race sample must be 16 bytes");
_Static_assert(sizeof(race_sample_v2_t) == RACE_SAMPLE_V2_SIZE, "race v2 sample must be 20 bytes");

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t hdr_hash(const race_hdr_t *h) {
    return fnv((const uint8_t *)h, (int)offsetof(race_hdr_t, hash));
}

static uint32_t hdr_hash2(const race_hdr_t *h) {
    return fnv((const uint8_t *)h, (int)offsetof(race_hdr_t, hash2));
}

static uint32_t slot_base(int slot) {
    return RACE_BASE + (uint32_t)slot * RACE_SLOT_SIZE;
}

static bool read_hdr(int slot, race_hdr_t *h) {
    if (slot < 0 || slot >= RACE_SLOTS) return false;
    if (!ext_flash_read(slot_base(slot), h, sizeof(*h))) return false;
    if (h->magic != RACE_MAGIC || h->valid != 1) return false;
    if (h->ver == 1) return h->hash == hdr_hash(h);
    if (h->ver == 2) return h->hash2 == hdr_hash2(h);
    return false;
}

static bool write_hdr(int slot, race_hdr_t *h) {
    if (slot < 0 || slot >= RACE_SLOTS) return false;
    s_hdr_cache_slot = -1;
    if (h->ver >= 2) h->hash2 = hdr_hash2(h);
    else h->hash = hdr_hash(h);
    if (!ext_flash_erase_sector(slot_base(slot))) return false;
    return ext_flash_write(slot_base(slot), h, sizeof(*h));
}

void race_store_migrate(void) {
    race_hdr_t *h = (race_hdr_t *)heap_caps_malloc(sizeof(race_hdr_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    race_hdr_t *v2 = (race_hdr_t *)heap_caps_malloc(sizeof(race_hdr_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!h || !v2) {
        ESP_LOGE(TAG, "session migration buffer allocation failed");
        free(h);
        free(v2);
        return;
    }
    for (int slot = 0; slot < RACE_SLOTS; slot++) {
        if (!read_hdr(slot, h)) continue;
        if (h->ver >= 2) continue;
        *v2 = *h;
        v2->ver = 2;
        v2->sample_size = RACE_SAMPLE_SIZE;  /* preserve legacy 16-byte stream */
        v2->end_epoch = h->start_epoch + (h->duration_ms / 1000u);
        v2->session_type = 1;
        rb_uuid_new(&v2->session_id);
        rb_uuid_clear(&v2->track_id);
        rb_uuid_clear(&v2->track_revision_id);
        v2->track_revision = 0;
        track_t *tr = (track_t *)heap_caps_malloc(sizeof(track_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (tr && h->track_slot < TSTORE_EXT_SLOTS && tstore_load(h->track_slot, tr)) {
            v2->track_id = tr->id;
            v2->track_revision_id = tr->rev_id;
            v2->track_revision = tr->revision;
        }
        free(tr);
        for (int i = 0; i < RACE_LAPS; i++) {
            v2->lap_sample_start[i] = 0;
            v2->lap_sample_end[i] = 0;
            v2->lap_sector_count[i] = 0;
            if (i < v2->lap_count) rb_uuid_new(&v2->lap_id[i]);
            else rb_uuid_clear(&v2->lap_id[i]);
            for (int s = 0; s < MAX_SECTIONS; s++) v2->lap_sector_ms[i][s] = 0;
        }
        if (write_hdr(slot, v2)) {
            ESP_LOGI(TAG, "migrated legacy session slot=%d to v2 metadata", slot);
        } else {
            ESP_LOGE(TAG, "session migration failed slot=%d", slot);
        }
    }
    free(h);
    free(v2);
}
static void hdr_cache_invalidate(void) {
    s_hdr_cache_slot = -1;
}

static bool hdr_cached(int slot, race_hdr_t *out) {
    if (slot == s_hdr_cache_slot) {
        *out = s_hdr_cache;
        return true;
    }
    if (!read_hdr(slot, &s_hdr_cache)) {
        s_hdr_cache_slot = -1;
        return false;
    }
    s_hdr_cache_slot = slot;
    *out = s_hdr_cache;
    return true;
}

int race_store_count(void) {
    int n = 0;
    race_hdr_t h;
    for (int i = 0; i < RACE_SLOTS; i++) if (read_hdr(i, &h)) n++;
    return n;
}

bool race_store_info(int slot, race_hdr_t *out) {
    return read_hdr(slot, out);
}

bool race_store_begin(int track_slot, const char *track_name, int mode,
                      const rb_uuid_t *track_id, const rb_uuid_t *track_revision_id,
                      uint32_t track_revision) {
    if (s_active) race_store_end(NULL);
    int slot = -1;
    race_hdr_t h;
    for (int i = 0; i < RACE_SLOTS; i++) {
        if (!read_hdr(i, &h)) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < RACE_SLOTS; i++) {
            if (!read_hdr(i, &h)) continue;
            if (h.lap_count == 0 || h.sample_count == 0) {
                slot = i;
                ESP_LOGW(TAG, "reclaiming empty session slot %d", slot);
                break;
            }
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no free session slot");
        return false;
    }
    hdr_cache_invalidate();
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.magic = RACE_MAGIC;
    s_hdr.ver = 2;
    s_hdr.valid = 0;
    s_hdr.id = (uint32_t)time(NULL);
    s_hdr.track_slot = (uint16_t)track_slot;
    s_hdr.mode = (uint8_t)mode;
    s_hdr.sample_size = RACE_SAMPLE_V2_SIZE;
    s_hdr.session_type = 1;  /* race; practice can be added by a future UI */
    rb_uuid_new(&s_hdr.session_id);
    if (track_id) s_hdr.track_id = *track_id; else rb_uuid_clear(&s_hdr.track_id);
    if (track_revision_id) s_hdr.track_revision_id = *track_revision_id;
    else rb_uuid_clear(&s_hdr.track_revision_id);
    s_hdr.track_revision = track_revision;
    if (track_name) {
        snprintf(s_hdr.track_name, sizeof(s_hdr.track_name), "%s", track_name);
    }
    s_hdr.start_epoch = (uint32_t)time(NULL);
    for (int i = 0; i < RACE_LAPS; i++) {
        s_lap_first[i] = 0xFFFFFFFFu;
        s_lap_last[i] = 0xFFFFFFFFu;
    }
    s_slot = slot;
    s_data = slot_base(slot) + RACE_HDR_SIZE;
    s_last_sector = 0xFFFFFFFFu;
    s_start_us = 0;
    s_last_fix_time_us = 0;
    s_last_fix_seq = 0;
    s_start_valid = false;
    s_active = true;
    /* The header is written only in race_store_end(). Until then the slot is
     * deliberately invalid; an interrupted/power-lost session never leaves a
     * half-valid header that can be mistaken for a completed session. */
    ESP_LOGI(TAG, "session start slot=%d track=%d '%s' mode=%d",
             slot, track_slot, s_hdr.track_name, mode);
    return true;
}

void race_store_sample(double lat, double lon, float speed_kmh, int lap, bool fix,
                       uint32_t fix_seq, uint64_t fix_time_us,
                       float accuracy_m, int satellites, int fix_type) {
    if (!s_active) return;
    if (!fix || (!fix_time_us && !fix_seq)) return;
    /* fix_seq advances only when GGA/RMC/NAV-PVT produced a new GNSS time;
     * GSV/GSA bytes must never create a duplicate sample. */
    if (fix_seq && fix_seq == s_last_fix_seq) return;
    if (!fix_seq && fix_time_us == s_last_fix_time_us) return;
    if (fix_seq) s_last_fix_seq = fix_seq;
    s_last_fix_time_us = fix_time_us;
    if (!s_start_valid) {
        s_start_us = fix_time_us;
        s_start_valid = true;
    }
    uint32_t max_samples = (RACE_SLOT_SIZE - RACE_HDR_SIZE) / RACE_SAMPLE_V2_SIZE;
    if (s_hdr.sample_count >= max_samples) {
        ESP_LOGW(TAG, "session slot full (%u samples)", s_hdr.sample_count);
        return;
    }
    uint32_t off = s_data + s_hdr.sample_count * RACE_SAMPLE_V2_SIZE;
    uint32_t sector = off & ~(RACE_HDR_SIZE - 1u);
    if (sector != s_last_sector) {
        if (!ext_flash_erase_sector(sector)) return;
        s_last_sector = sector;
    }
    race_sample_v2_t sm;
    int64_t dt_us = (int64_t)fix_time_us - (int64_t)s_start_us;
    if (dt_us < -43200000000LL) dt_us += 86400000000LL;  /* midnight rollover */
    if (dt_us < 0) dt_us = 0;
    memset(&sm, 0, sizeof(sm));
    sm.t_ms = (uint32_t)(dt_us / 1000);
    sm.lat_e7 = (int32_t)(lat * 1e7);
    sm.lon_e7 = (int32_t)(lon * 1e7);
    sm.speed10 = (uint16_t)(speed_kmh * 10.0f + 0.5f);
    sm.accuracy_cm = accuracy_m > 0.0f
        ? (uint16_t)fminf(accuracy_m * 100.0f, 65534.0f) : 0xFFFFu;
    sm.sats = (uint8_t)(satellites > 255 ? 255 : (satellites < 0 ? 0 : satellites));
    sm.fix_type = (uint8_t)(fix_type > 255 ? 255 : (fix_type < 0 ? 0 : fix_type));
    sm.lap = (uint8_t)(lap > 255 ? 255 : (lap < 0 ? 0 : lap));
    sm.flags = 1;
    if (lap >= 1 && lap <= RACE_LAPS) {
        int li = lap - 1;
        if (s_lap_first[li] == 0xFFFFFFFFu) s_lap_first[li] = s_hdr.sample_count;
        s_lap_last[li] = s_hdr.sample_count;
    }
    if (!ext_flash_write(off, &sm, sizeof(sm))) return;
    s_hdr.sample_count++;
}

void race_store_end(const engine_t *e) {
    if (!s_active) return;
    if (e) {
        s_hdr.duration_ms = (uint32_t)(e->time * 1000.0f);
        s_hdr.lap_count = (uint8_t)(e->nLaps > RACE_LAPS ? RACE_LAPS : e->nLaps);
        for (int i = 0; i < s_hdr.lap_count; i++) {
            s_hdr.lap_time[i] = e->laps[i].time;
            s_hdr.lap_samples[i] = (uint32_t)e->laps[i].nGps;
        }
    }
    s_hdr.valid = 1;
    s_hdr.end_epoch = s_hdr.start_epoch + (s_hdr.duration_ms / 1000u);
    s_hdr.sample_size = RACE_SAMPLE_V2_SIZE;
    s_hdr.sector_count = 0;
    for (int i = 0; i < RACE_LAPS; i++) {
        s_hdr.lap_sample_start[i] = s_lap_first[i];
        s_hdr.lap_sample_end[i] = s_lap_last[i];
        s_hdr.lap_sector_count[i] = 0;
        if (i < s_hdr.lap_count) rb_uuid_new(&s_hdr.lap_id[i]);
        else rb_uuid_clear(&s_hdr.lap_id[i]);
    }
    if (s_hdr.duration_ms && s_hdr.sample_count) {
        s_hdr.sample_rate_hz = (uint16_t)((s_hdr.sample_count * 1000u) / s_hdr.duration_ms);
    }
    bool ok = write_hdr(s_slot, &s_hdr);
    ESP_LOGI(TAG, "session end slot=%d samples=%u laps=%u ok=%d",
             s_slot, s_hdr.sample_count, s_hdr.lap_count, ok);
    s_active = false;
    s_slot = -1;
}

bool race_store_active(void) { return s_active; }

void race_store_abort(void) {
    hdr_cache_invalidate();
    if (s_slot >= 0) ext_flash_erase_sector(slot_base(s_slot));
    s_active = false;
    s_slot = -1;
}

bool race_store_read_sample_v2(int slot, int index, race_sample_v2_t *out) {
    race_hdr_t h;
    if (!out || !hdr_cached(slot, &h) || index < 0 || (uint32_t)index >= h.sample_count) return false;
    if (h.ver >= 2 && h.sample_size == RACE_SAMPLE_V2_SIZE) {
        uint32_t off = slot_base(slot) + RACE_HDR_SIZE +
                       (uint32_t)index * RACE_SAMPLE_V2_SIZE;
        return ext_flash_read(off, out, sizeof(*out));
    }
    race_sample_t old;
    uint32_t off = slot_base(slot) + RACE_HDR_SIZE + (uint32_t)index * RACE_SAMPLE_SIZE;
    if (!ext_flash_read(off, &old, sizeof(old))) return false;
    memset(out, 0, sizeof(*out));
    out->t_ms = old.t_ms;
    out->lat_e7 = old.lat_e7;
    out->lon_e7 = old.lon_e7;
    out->speed10 = old.speed10;
    out->accuracy_cm = 0xFFFFu;
    out->sats = 0;
    out->fix_type = (old.flags & 1) ? 2 : 0;
    out->lap = old.lap;
    out->flags = old.flags;
    return true;
}

bool race_store_read_sample(int slot, int index, race_sample_t *out) {
    race_sample_v2_t v;
    if (!out || !race_store_read_sample_v2(slot, index, &v)) return false;
    out->t_ms = v.t_ms;
    out->lat_e7 = v.lat_e7;
    out->lon_e7 = v.lon_e7;
    out->speed10 = v.speed10;
    out->lap = v.lap;
    out->flags = v.flags;
    return true;
}


int race_store_rename(const char *old_name, const char *new_name) {
    if (!old_name || !new_name || !new_name[0]) return 0;
    int changed = 0;
    race_hdr_t h;
    for (int i = 0; i < RACE_SLOTS; i++) {
        if (!read_hdr(i, &h)) continue;
        if (strcmp(h.track_name, old_name) != 0) continue;
        snprintf(h.track_name, sizeof(h.track_name), "%s", new_name);
        if (write_hdr(i, &h)) changed++;
    }
    return changed;
}

bool race_store_delete(int slot) {
    race_hdr_t h;
    if (!read_hdr(slot, &h)) return false;
    h.valid = 0;
    return write_hdr(slot, &h);
}

bool race_store_delete_lap(int slot, int lap) {
    race_hdr_t h;
    if (!read_hdr(slot, &h) || lap < 1 || lap > RACE_LAPS) return false;
    h.lap_deleted_mask |= (uint16_t)(1u << (lap - 1));
    h.lap_time[lap - 1] = 0;
    h.lap_samples[lap - 1] = 0;
    rb_uuid_clear(&h.lap_id[lap - 1]);
    h.lap_sector_count[lap - 1] = 0;
    for (int i = 0; i < MAX_SECTIONS; i++) h.lap_sector_ms[lap - 1][i] = 0;
    return write_hdr(slot, &h);
}
