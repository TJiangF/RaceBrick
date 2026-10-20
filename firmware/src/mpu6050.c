#include "mpu6050.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "mpu6050";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_ok = false;

static bool wr(uint8_t reg, uint8_t val) {
    if (!s_dev) return false;
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 50) == ESP_OK;
}

static bool rd(uint8_t reg, uint8_t *buf, int n) {
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50) == ESP_OK;
}

bool mpu6050_init(void) {
    if (!s_bus) {
        i2c_master_bus_config_t bc = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
        if (e != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(e)); return false; }
    }

    for (int a = 0; a < 2 && !s_ok; a++) {
        uint8_t addr = (a == 0) ? 0x68 : 0x69;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addr,
            .scl_speed_hz = 400000,
        };
        i2c_master_dev_handle_t dev = NULL;
        if (i2c_master_bus_add_device(s_bus, &dc, &dev) != ESP_OK) continue;
        s_dev = dev;
        uint8_t who = 0;
        if (rd(0x75, &who, 1) && who == 0x68) {
            wr(0x6B, 0x00);   /* PWR_MGMT_1: wake */
            wr(0x19, 0x07);   /* SMPLRT_DIV: 1k/(1+7) = 125 Hz */
            wr(0x1A, 0x03);   /* CONFIG: DLPF ~44 Hz */
            wr(0x1B, 0x00);   /* GYRO_CONFIG: +/-250 deg/s */
            wr(0x1C, 0x00);   /* ACCEL_CONFIG: +/-2 g */
            s_ok = true;
            ESP_LOGI(TAG, "MPU6050 found @0x%02X (WHO_AM_I=0x%02X)", addr, who);
            return true;
        }
        ESP_LOGW(TAG, "no MPU6050 @0x%02X (WHO_AM_I=0x%02X)", addr, who);
    }
    s_ok = false;
    return false;
}

bool mpu6050_present(void) { return s_ok; }

bool mpu6050_read(float *ax, float *ay, float *az, float *gx, float *gy, float *gz, float *temp_c) {
    if (!s_ok) return false;
    uint8_t b[14];
    if (!rd(0x3B, b, 14)) return false;
    int16_t rax = (int16_t)((b[0] << 8) | b[1]);
    int16_t ray = (int16_t)((b[2] << 8) | b[3]);
    int16_t raz = (int16_t)((b[4] << 8) | b[5]);
    int16_t rt  = (int16_t)((b[6] << 8) | b[7]);
    int16_t rgx = (int16_t)((b[8] << 8) | b[9]);
    int16_t rgy = (int16_t)((b[10] << 8) | b[11]);
    int16_t rgz = (int16_t)((b[12] << 8) | b[13]);
    *ax = rax / 16384.0f; *ay = ray / 16384.0f; *az = raz / 16384.0f;
    *gx = rgx / 131.0f;   *gy = rgy / 131.0f;   *gz = rgz / 131.0f;
    *temp_c = rt / 340.0f + 36.53f;
    return true;
}
