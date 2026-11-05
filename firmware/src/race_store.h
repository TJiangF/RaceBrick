/* Raw race-session store on the external W25Q128.
 *
 * Layout: 7 fixed 2 MiB slots starting at 0x100000. Each slot has a 4 KiB
 * header followed by raw 16-byte GPS samples. No compression or layering.
 * 2 MiB supports ~130,800 samples = ~87 minutes at 25 Hz.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "sim.h"

#define RACE_SLOTS       7
#define RACE_SLOT_SIZE   0x200000u
#define RACE_BASE        0x100000u
#define RACE_HDR_SIZE    4096u
#define RACE_LAPS        16
#define RACE_SAMPLE_SIZE 16u

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
    uint32_t hash;
} race_hdr_t;

typedef struct {
    uint32_t t_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t speed10;
    uint8_t  lap;
    uint8_t  flags;       /* bit0 = valid 3D fix */
} race_sample_t;

bool race_store_begin(int track_slot, const char *track_name, int mode);
void race_store_sample(double lat, double lon, float speed_kmh, int lap, bool fix, uint64_t fix_us);
void race_store_end(const engine_t *e);
bool race_store_active(void);

int  race_store_count(void);
bool race_store_info(int slot, race_hdr_t *out);
bool race_store_read_sample(int slot, int index, race_sample_t *out);
bool race_store_delete(int slot);
bool race_store_delete_lap(int slot, int lap);
