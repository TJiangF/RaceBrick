/* External SPI NOR flash (W25Q128) on the production board.
 * Separate SPI3 bus: CS=39, MISO=38, SCK=37, MOSI=36.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ext_flash_selftest(void);       /* init + JEDEC id + erase/write/read */
bool ext_flash_read_jedec(uint8_t id[3]);

/* data access (W25Q128 = 16 MiB) */
bool ext_flash_init(void);
bool ext_flash_erase_sector(uint32_t addr);
bool ext_flash_read(uint32_t addr, void *buf, int n);
bool ext_flash_write(uint32_t addr, const void *buf, int n);
uint32_t ext_flash_size(void);
