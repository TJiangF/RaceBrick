/* Persistent track store on the external W25Q128 (16 MiB, SPI3).
 * Fixed slots of 4 KiB starting at 0x10000 (sector 0 is used by the boot
 * self-test). Each slot holds one serialised track + FNV hash. */
#pragma once
#include <stdbool.h>
#include "sim.h"

#define TSTORE_EXT_SLOTS 256
#define TSTORE_INT_SLOTS 5

bool tstore_begin(void);              /* init flash + migrate + count heads */
int  tstore_count(void);
int  tstore_capacity(void);
int  tstore_first_free(void);         /* -1 when full */
int  tstore_slots(int *out, int cap); /* current head slots, fastest path for HTTP */
bool tstore_load(int slot, track_t *t);
bool tstore_save(int slot, track_t *t);       /* new record / migration */
bool tstore_commit(int *slot_io, track_t *t); /* create a new revision */
int  tstore_head_slots(int *out, int cap);    /* current track heads only */
bool tstore_delete(int slot);
