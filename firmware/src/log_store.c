#include "log_store.h"
#include "extflash.h"
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_MAGIC 0x524C4F47u  /* 'RLOG' */
static const char *TAG = "logstore";
static int s_count = -1;
static SemaphoreHandle_t s_lock;

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t reserved;
    uint32_t hash;
    histrec_t rec;
} log_rec_t;

_Static_assert(sizeof(log_rec_t) <= LOG_SLOT_SIZE, "race log record exceeds slot");

/* log_rec_t is ~1.6 KiB. Keeping two of them on the 3.5 KiB main-task stack
 * overflowed when KEEP RACE LOG? called log_store_save(). Use BSS scratch. */
static log_rec_t s_io;
static log_rec_t s_tx;

static void log_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

static void log_unlock(void) {
    if (s_lock) xSemaphoreGiveRecursive(s_lock);
}

static uint32_t fnv(const uint8_t *p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t slot_addr(int slot) {
    return LOG_BASE + (uint32_t)slot * LOG_SLOT_SIZE;
}

static bool valid_rec(const log_rec_t *r) {
    if (r->magic != LOG_MAGIC || r->ver != 1) return false;
    return r->hash == fnv((const uint8_t *)r, (int)offsetof(log_rec_t, hash));
}

int log_store_capacity(void) { return LOG_SLOTS; }
int log_store_slot_size(void) { return (int)LOG_SLOT_SIZE; }
uint32_t log_store_used_bytes(void) { return (uint32_t)log_store_count() * LOG_SLOT_SIZE; }
uint32_t log_store_total_bytes(void) { return (uint32_t)LOG_SLOTS * LOG_SLOT_SIZE; }

int log_store_count(void) {
    log_lock();
    if (s_count >= 0) { log_unlock(); return s_count; }
    int n = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (ext_flash_read(slot_addr(i), &s_io, sizeof(s_io)) && valid_rec(&s_io)) n++;
    }
    s_count = n;
    log_unlock();
    return n;
}

int log_store_load(histrec_t *out, int cap) {
    log_lock();
    if (!out || cap <= 0) { log_unlock(); return 0; }
    int total = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (ext_flash_read(slot_addr(i), &s_io, sizeof(s_io)) && valid_rec(&s_io)) total++;
    }
    int skip = total > cap ? total - cap : 0;
    int n = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (!ext_flash_read(slot_addr(i), &s_io, sizeof(s_io)) || !valid_rec(&s_io)) continue;
        if (skip) { skip--; continue; }
        out[n++] = s_io.rec;
    }
    s_count = total;
    ESP_LOGI(TAG, "loaded %d/%d logs, %u/%u KiB used", n, total,
             (unsigned)((uint32_t)total * LOG_SLOT_SIZE / 1024),
             (unsigned)((uint32_t)LOG_SLOTS * LOG_SLOT_SIZE / 1024));
    log_unlock();
    return n;
}



bool log_store_clear(void) {
    log_lock();
    bool ok = true;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (!ext_flash_erase_sector(slot_addr(i))) ok = false;
    }
    s_count = 0;
    log_unlock();
    return ok;
}

int log_store_rename(const char *old_name, const char *new_name) {
    log_lock();
    if (!old_name || !new_name || !new_name[0]) { log_unlock(); return 0; }
    int changed = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (!ext_flash_read(slot_addr(i), &s_io, sizeof(s_io)) || !valid_rec(&s_io)) continue;
        if (strcmp(s_io.rec.trackName, old_name) != 0) continue;
        snprintf(s_io.rec.trackName, sizeof(s_io.rec.trackName), "%s", new_name);
        s_io.hash = fnv((const uint8_t *)&s_io, (int)offsetof(log_rec_t, hash));
        if (ext_flash_erase_sector(slot_addr(i)) &&
            ext_flash_write(slot_addr(i), &s_io, sizeof(s_io))) changed++;
    }
    if (changed) { s_count = -1; log_store_count(); }
    log_unlock();
    return changed;
}

bool log_store_save(const histrec_t *rec_in) {
    log_lock();
    if (!rec_in) { log_unlock(); return false; }
    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.magic = LOG_MAGIC;
    s_tx.ver = 1;
    s_tx.rec = *rec_in;
    s_tx.hash = fnv((const uint8_t *)&s_tx, (int)offsetof(log_rec_t, hash));

    for (int i = 0; i < LOG_SLOTS; i++) {
        if (ext_flash_read(slot_addr(i), &s_io, sizeof(s_io)) && valid_rec(&s_io)) continue;
        if (!ext_flash_erase_sector(slot_addr(i))) {
            ESP_LOGE(TAG, "erase slot %d failed", i);
            log_unlock();
            return false;
        }
        if (!ext_flash_write(slot_addr(i), &s_tx, sizeof(s_tx))) {
            ESP_LOGE(TAG, "write slot %d failed", i);
            log_unlock();
            return false;
        }
        if (s_count < 0) log_store_count();
        if (s_count < LOG_SLOTS) s_count++;
        ESP_LOGI(TAG, "saved '%s' to slot %d (%d bytes)", s_tx.rec.trackName, i, (int)sizeof(s_tx));
        log_unlock();
        return true;
    }
    ESP_LOGW(TAG, "race log flash is full");
    log_unlock();
    return false;
}
