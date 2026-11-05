#include "race_store.h"
#include "extflash.h"
#include "config.h"
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "racestore";
#define RACE_MAGIC 0x52534331u  /* 'RSC1' */

static bool s_active;
static int s_slot = -1;
static race_hdr_t s_hdr;
static uint32_t s_data;
static uint32_t s_last_sector = 0xFFFFFFFFu;
static uint64_t s_start_us;
static uint64_t s_last_fix_us;

_Static_assert(sizeof(race_hdr_t) <= RACE_HDR_SIZE, "race header too large");
_Static_assert(sizeof(race_sample_t) == RACE_SAMPLE_SIZE, "race sample must be 16 bytes");

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t hdr_hash(const race_hdr_t *h) {
    return fnv((const uint8_t *)h, (int)offsetof(race_hdr_t, hash));
}

static uint32_t slot_base(int slot) {
    return RACE_BASE + (uint32_t)slot * RACE_SLOT_SIZE;
}

static bool read_hdr(int slot, race_hdr_t *h) {
    if (slot < 0 || slot >= RACE_SLOTS) return false;
    if (!ext_flash_read(slot_base(slot), h, sizeof(*h))) return false;
    return h->magic == RACE_MAGIC && h->ver == 1 &&
           h->valid == 1 && h->hash == hdr_hash(h);
}

static bool write_hdr(int slot, race_hdr_t *h) {
    if (slot < 0 || slot >= RACE_SLOTS) return false;
    h->hash = hdr_hash(h);
    if (!ext_flash_erase_sector(slot_base(slot))) return false;
    return ext_flash_write(slot_base(slot), h, sizeof(*h));
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

bool race_store_begin(int track_slot, const char *track_name, int mode) {
    if (s_active) race_store_end(NULL);
    int slot = -1;
    race_hdr_t h;
    for (int i = 0; i < RACE_SLOTS; i++) {
        if (!read_hdr(i, &h)) { slot = i; break; }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "no free session slot");
        return false;
    }
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr.magic = RACE_MAGIC;
    s_hdr.ver = 1;
    s_hdr.valid = 0;
    s_hdr.id = (uint32_t)time(NULL);
    s_hdr.track_slot = (uint16_t)track_slot;
    s_hdr.mode = (uint8_t)mode;
    if (track_name) {
        strncpy(s_hdr.track_name, track_name, sizeof(s_hdr.track_name) - 1);
    }
    s_hdr.start_epoch = (uint32_t)time(NULL);
    s_slot = slot;
    s_data = slot_base(slot) + RACE_HDR_SIZE;
    s_last_sector = 0xFFFFFFFFu;
    s_start_us = esp_timer_get_time();
    s_last_fix_us = 0;
    s_active = true;
    if (!ext_flash_erase_sector(slot_base(slot)) ||
        !ext_flash_write(slot_base(slot), &s_hdr, sizeof(s_hdr))) {
        s_active = false;
        ESP_LOGE(TAG, "session header init failed");
        return false;
    }
    ESP_LOGI(TAG, "session start slot=%d track=%d '%s' mode=%d",
             slot, track_slot, s_hdr.track_name, mode);
    return true;
}

void race_store_sample(double lat, double lon, float speed_kmh, int lap, bool fix, uint64_t fix_us) {
    if (!s_active) return;
    if (!fix) return;
    if (fix_us && fix_us == s_last_fix_us) return;
    s_last_fix_us = fix_us;
    uint32_t max_samples = (RACE_SLOT_SIZE - RACE_HDR_SIZE) / RACE_SAMPLE_SIZE;
    if (s_hdr.sample_count >= max_samples) {
        ESP_LOGW(TAG, "session slot full (%u samples)", s_hdr.sample_count);
        return;
    }
    uint32_t off = s_data + s_hdr.sample_count * RACE_SAMPLE_SIZE;
    uint32_t sector = off & ~(RACE_HDR_SIZE - 1u);
    if (sector != s_last_sector) {
        if (!ext_flash_erase_sector(sector)) return;
        s_last_sector = sector;
    }
    race_sample_t sm;
    uint64_t now = esp_timer_get_time();
    sm.t_ms = (uint32_t)((now - s_start_us) / 1000);
    sm.lat_e7 = (int32_t)(lat * 1e7);
    sm.lon_e7 = (int32_t)(lon * 1e7);
    sm.speed10 = (uint16_t)(speed_kmh * 10.0f + 0.5f);
    sm.lap = (uint8_t)(lap > 255 ? 255 : (lap < 0 ? 0 : lap));
    sm.flags = 1;
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

bool race_store_read_sample(int slot, int index, race_sample_t *out) {
    race_hdr_t h;
    if (!read_hdr(slot, &h) || index < 0 || (uint32_t)index >= h.sample_count) return false;
    uint32_t off = slot_base(slot) + RACE_HDR_SIZE + (uint32_t)index * RACE_SAMPLE_SIZE;
    return ext_flash_read(off, out, sizeof(*out));
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
    return write_hdr(slot, &h);
}
