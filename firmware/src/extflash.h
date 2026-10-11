/* External SPI NOR flash (W25Q128) on the production board.
 * Separate SPI3 bus: CS=39, MISO=38, SCK=37, MOSI=36.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ext_flash_selftest(void);       /* init + JEDEC id + erase/write/read */
bool ext_flash_read_jedec(uint8_t id[3]);
