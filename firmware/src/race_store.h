/* Raw race-session store on the external W25Q128.
 *
 * Layout: 7 fixed 2 MiB slots starting at 0x100000. Each slot has a 4 KiB
 * header followed by raw GPS samples. Legacy sessions use 16-byte samples;
 * new v2 sessions use 20-byte samples (accuracy/satellites/fix type).
 * 2 MiB supports ~104,800 v2 samples = ~70 minutes at 25 Hz.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sim.h"
#include "rb_uuid.h"

#define RACE_SLOTS       7
#define RACE_SLOT_SIZE   0x200000u
#define RACE_BASE        0x100000u
#define RACE_HDR_SIZE    4096u
#define RACE_LAPS        16
#define RACE_SAMPLE_SIZE 16u
#define RACE_SAMPLE_V2_SIZE 20u

typedef struct {
    uint32_t magic;
    uint16_t ver;
    uint16_t valid;
    uint32_t id;
    uint16_t track_slot;
    uint8_t  mode;
    uint8_t  lap_count;
    char     track_name[12];
    char     date[20];
    uint32_t start_epoch;
    uint32_t duration_ms;
    uint32_t sample_count;
    uint16_t sample_rate_hz;
    uint16_t reserved;
    float    lap_time[RACE_LAPS];
    uint32_t lap_samples[RACE_LAPS];
    uint16_t lap_deleted_mask;
    uint8_t  pad[2];
    uint32_t hash;              /* v1 hash; kept only for backward validation */

    /* v2 extension (all new sessions) */
    rb_uuid_t session_id;
    rb_uuid_t track_id;
    rb_uuid_t track_revision_id;
    uint32_t track_revision;
    uint32_t end_epoch;
    uint8_t  sample_size;       /* 20 for v2, 0 means legacy 16-byte */
    uint8_t  session_type;      /* 1 race, 2 practice, 0 unknown */
    uint8_t  sector_count;
    uint8_t  reserved2;
    uint32_t lap_sample_start[RACE_LAPS];
    uint32_t lap_sample_end[RACE_LAPS];
    rb_uuid_t lap_id[RACE_LAPS];
    uint16_t lap_sector_ms[RACE_LAPS][MAX_SECTIONS];
    uint8_t  lap_sector_count[RACE_LAPS];
    uint8_t  pad2[3];
    uint32_t hash2;
    uint8_t  data_hash[32];     /* SHA-256 over header metadata + raw samples */
} race_hdr_t;

typedef struct {
    uint32_t t_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t speed10;
    uint8_t  lap;
    uint8_t  flags;       /* bit0 = valid 3D fix */
} race_sample_t;

typedef struct __attribute__((packed)) {
    uint32_t t_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t speed10;
    uint16_t accuracy_cm; /* horizontal accuracy, cm; 0xFFFF = unavailable */
    uint8_t  sats;
    uint8_t  fix_type;
    uint8_t  lap;
    uint8_t  flags;       /* bit0 = valid 3D fix */
} race_sample_v2_t;

bool race_store_begin(int track_slot, const char *track_name, int mode,
                      const rb_uuid_t *track_id, const rb_uuid_t *track_revision_id,
                      uint32_t track_revision);
void race_store_sample(double lat, double lon, float speed_kmh, int lap, bool fix,
                       uint32_t fix_seq, uint64_t fix_time_us,
                       float accuracy_m, int satellites, int fix_type);
void race_store_end(const engine_t *e);
bool race_store_active(void);
void race_store_abort(void);

void race_store_migrate(void);
void race_store_hash_migrate(void);
int  race_store_count(void);
bool race_store_info(int slot, race_hdr_t *out);
bool race_store_read_sample(int slot, int index, race_sample_t *out);
bool race_store_read_sample_v2(int slot, int index, race_sample_v2_t *out);
bool race_store_delete(int slot);
int race_store_rename(const char *old_name, const char *new_name);
bool race_store_delete_lap(int slot, int lap);
