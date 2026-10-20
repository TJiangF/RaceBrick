/* Persistent track store on the external W25Q128 (16 MiB, SPI3).
 * Fixed slots of 4 KiB starting at 0x10000 (sector 0 is used by the boot
 * self-test). Each slot holds one serialised track + FNV hash. */
#pragma once
#include <stdbool.h>
#include "sim.h"

#define TSTORE_SLOTS 5

bool tstore_begin(void);              /* init flash + count valid slots */
int  tstore_count(void);
int  tstore_capacity(void);
int  tstore_first_free(void);         /* -1 when full */
bool tstore_load(int slot, track_t *t);
bool tstore_save(int slot, const track_t *t);
