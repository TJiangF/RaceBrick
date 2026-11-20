/* Race-log persistence in the final 1 MiB of the external 16 MiB flash.
 *
 * External allocation:
 *   0x000000-0x0FFFFF  tracks (256 x 4 KiB)
 *   0x100000-0xEFFFFF  race sessions (7 x 2 MiB)
 *   0xF00000-0xFFFFFF  race logs (256 x 4 KiB)
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app.h"

#define LOG_BASE        0xF00000u
#define LOG_SLOTS       256
#define LOG_SLOT_SIZE   0x1000u

int  log_store_count(void);
int  log_store_capacity(void);
int  log_store_slot_size(void);
uint32_t log_store_used_bytes(void);
uint32_t log_store_total_bytes(void);
int  log_store_load(histrec_t *out, int cap); /* valid records, oldest first */
bool log_store_save(const histrec_t *rec);     /* first free slot */
int  log_store_rename(const char *old_name, const char *new_name);
bool log_store_clear(void);
