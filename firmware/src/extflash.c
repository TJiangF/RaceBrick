#include "extflash.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "extflash";

/* W25Q commands */
#define CMD_WREN   0x06
#define CMD_RDSR1  0x05
#define CMD_READ   0x03
#define CMD_PP     0x02
#define CMD_SE     0x20
#define CMD_RDID   0x9F

static spi_device_handle_t s_dev;
static bool s_init = false;

static bool xfer(const uint8_t *tx, uint8_t *rx, int n) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(s_dev, &t) == ESP_OK;
}

static bool cmd_tx(uint8_t cmd) { return xfer(&cmd, NULL, 1); }

static uint8_t read_sr1(void) {
    uint8_t tx[2] = {CMD_RDSR1, 0};
    uint8_t rx[2] = {0, 0};
    xfer(tx, rx, 2);
    return rx[1];
}

static bool wait_ready(int timeout_ms) {
    int waited = 0;
    while ((read_sr1() & 0x01) && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(1));
        waited++;
    }
    return (read_sr1() & 0x01) == 0;
}

bool ext_flash_read_jedec(uint8_t id[3]) {
    if (!s_init) { id[0] = id[1] = id[2] = 0; return false; }
    uint8_t tx[4] = {CMD_RDID, 0, 0, 0};
    uint8_t rx[4] = {0};
    if (!xfer(tx, rx, 4)) return false;
    id[0] = rx[1]; id[1] = rx[2]; id[2] = rx[3];
    return true;
}

static bool erase_sector(uint32_t addr) {
    if (!cmd_tx(CMD_WREN)) return false;
    uint8_t tx[4] = {CMD_SE, (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    if (!xfer(tx, NULL, 4)) return false;
    return wait_ready(1000);
}

static bool page_program(uint32_t addr, const uint8_t *data, int n) {
    if (!cmd_tx(CMD_WREN)) return false;
    uint8_t tx[4 + 256];
    tx[0] = CMD_PP;
    tx[1] = (uint8_t)(addr >> 16);
    tx[2] = (uint8_t)(addr >> 8);
    tx[3] = (uint8_t)addr;
    memcpy(&tx[4], data, n);
    if (!xfer(tx, NULL, 4 + n)) return false;
    return wait_ready(500);
}

#define EXT_FLASH_MAX_XFER 3072
static uint8_t *s_rdbuf;   /* heap: keeps ~3 KB out of .bss/DRAM */

static bool read_data(uint32_t addr, uint8_t *buf, int n) {
    if (n > EXT_FLASH_MAX_XFER) return false;
    if (!s_rdbuf) {
        s_rdbuf = (uint8_t *)malloc(4 + EXT_FLASH_MAX_XFER);
        if (!s_rdbuf) return false;
    }
    s_rdbuf[0] = CMD_READ;
    s_rdbuf[1] = (uint8_t)(addr >> 16);
    s_rdbuf[2] = (uint8_t)(addr >> 8);
    s_rdbuf[3] = (uint8_t)addr;
    memset(s_rdbuf + 4, 0, n);
    if (!xfer(s_rdbuf, s_rdbuf, 4 + n)) return false;
    memcpy(buf, s_rdbuf + 4, n);
    return true;
}

/* write across page boundaries */
static bool write_data(uint32_t addr, const uint8_t *data, int n) {
    int off = 0;
    while (off < n) {
        int page_rem = 256 - (int)((addr + off) & 0xFF);
        int chunk = (n - off) < page_rem ? (n - off) : page_rem;
        if (!page_program(addr + off, data + off, chunk)) return false;
        off += chunk;
    }
    return true;
}

bool ext_flash_init(void) {
    if (s_init) return true;
    spi_bus_config_t bus = {
        .mosi_io_num = EXT_FLASH_MOSI,
        .miso_io_num = EXT_FLASH_MISO,
        .sclk_io_num = EXT_FLASH_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t e = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) { ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(e)); return false; }
    spi_device_interface_config_t d = {
        .clock_speed_hz = EXT_FLASH_HZ,
        .mode = 0,
        .spics_io_num = EXT_FLASH_CS,
        .queue_size = 1,
    };
    e = spi_bus_add_device(SPI3_HOST, &d, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(e)); return false; }
    s_init = true;
    return true;
}

bool ext_flash_erase_sector(uint32_t addr) { return s_init && erase_sector(addr); }
bool ext_flash_read(uint32_t addr, void *buf, int n) { return s_init && read_data(addr, (uint8_t *)buf, n); }
bool ext_flash_write(uint32_t addr, const void *buf, int n) { return s_init && write_data(addr, (const uint8_t *)buf, n); }
uint32_t ext_flash_size(void) { return 16u * 1024u * 1024u; }   /* W25Q128 */

bool ext_flash_selftest(void) {
    if (!ext_flash_init()) return false;

    uint8_t id[3];
    ext_flash_read_jedec(id);
    ESP_LOGI(TAG, "JEDEC ID = %02X %02X %02X (expect EF 40 18 for W25Q128)", id[0], id[1], id[2]);

    bool is_w25q128 = (id[0] == 0xEF && id[1] == 0x40 && id[2] == 0x18);
    if (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00) {
        ESP_LOGE(TAG, "no response from flash (all zero) - check wiring/CS/power");
        return false;
    }
    if (id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) {
        ESP_LOGE(TAG, "bus reads 0xFF (MISO idle high / no chip) - check MISO/CS");
        return false;
    }

    /* erase + write + read-verify a 256-byte block at offset 0x100 */
    const uint32_t base = 0x00000100;
    uint8_t wbuf[256], rbuf[256];
    for (int i = 0; i < 256; i++) wbuf[i] = (uint8_t)(i * 7 + 0x5A);

    ESP_LOGI(TAG, "erasing sector 0 ...");
    if (!erase_sector(0)) { ESP_LOGE(TAG, "erase timeout"); return false; }
    if (!write_data(base, wbuf, 256)) { ESP_LOGE(TAG, "program failed"); return false; }
    if (!read_data(base, rbuf, 256)) { ESP_LOGE(TAG, "read failed"); return false; }

    if (memcmp(wbuf, rbuf, 256) != 0) {
        ESP_LOGE(TAG, "verify mismatch");
        for (int i = 0; i < 256; i++) {
            if (wbuf[i] != rbuf[i]) { ESP_LOGE(TAG, "  first diff @%d: w=%02X r=%02X", i, wbuf[i], rbuf[i]); break; }
        }
        return false;
    }

    ESP_LOGI(TAG, "erase/write/read OK; %s", is_w25q128 ? "W25Q128 detected" : "ID unexpected");
    return is_w25q128;
}
