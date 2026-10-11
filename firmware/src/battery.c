#include "battery.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "batt";

#define VBAT_EN_GPIO  PIN_VBAT_EN     /* 21 */
#define VBAT_ADC_CH   ADC_CHANNEL_0   /* GPIO1 = ADC1_CH0 on ESP32-S3 */
#define VBAT_ATTEN    ADC_ATTEN_DB_12 /* ~0..3.3V full scale */

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;
static bool s_cali_ok = false;
static int  s_mv = 0;
static int64_t s_last = 0;

void battery_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << VBAT_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(VBAT_EN_GPIO, 0);   /* normally low */

    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&uc, &s_adc) != ESP_OK) { ESP_LOGE(TAG, "adc unit failed"); return; }
    adc_oneshot_chan_cfg_t cc = { .atten = VBAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_oneshot_config_channel(s_adc, VBAT_ADC_CH, &cc);

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1,
        .atten = VBAT_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
    ESP_LOGI(TAG, "battery sense ready (cali=%d)", (int)s_cali_ok);
}

void battery_update(void) {
    if (!s_adc) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last < 1000000) return;   /* ~1 Hz */
    s_last = now;

    gpio_set_level(VBAT_EN_GPIO, 1);      /* connect divider */
    vTaskDelay(pdMS_TO_TICKS(5));         /* let it settle */
    int sum = 0, n = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, VBAT_ADC_CH, &raw) == ESP_OK) { sum += raw; n++; }
    }
    gpio_set_level(VBAT_EN_GPIO, 0);      /* back to low */

    if (!n) return;
    int raw = sum / n;
    int mv = 0;
    if (s_cali_ok) adc_cali_raw_to_voltage(s_cali, raw, &mv);
    else mv = raw * 3300 / 4095;
    s_mv = mv * 2;                        /* divider 1:2 -> real battery voltage */
    ESP_LOGI(TAG, "VBAT %d mV (raw=%d, pin=%d mV)", s_mv, raw, mv);
}

int battery_mv(void) { return s_mv; }
