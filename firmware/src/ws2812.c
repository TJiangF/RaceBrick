#include "ws2812.h"
#include "config.h"
#include <string.h>
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "ws2812";

#define LED_N LED_COUNT
#define T0H 4   /* 0.4us @ 10MHz */
#define T0L 8
#define T1H 8   /* 0.8us */
#define T1L 4

static rmt_channel_handle_t s_tx = NULL;
static rmt_encoder_handle_t s_enc = NULL;
static bool s_ok = false;

static uint8_t s_r[LED_N], s_g[LED_N], s_b[LED_N];
static int s_bright = START_LED_BRIGHT;
static rmt_symbol_word_t s_sym[LED_N * 24];

void ws2812_init(void) {
    rmt_tx_channel_config_t tc = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,   /* 0.1us/tick */
        .mem_block_symbols = 256,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tc, &s_tx) != ESP_OK) { ESP_LOGE(TAG, "tx channel failed"); return; }
    rmt_copy_encoder_config_t ec = {};
    if (rmt_new_copy_encoder(&ec, &s_enc) != ESP_OK) { ESP_LOGE(TAG, "encoder failed"); return; }
    if (rmt_enable(s_tx) != ESP_OK) { ESP_LOGE(TAG, "enable failed"); return; }
    s_ok = true;
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d (%d LEDs)", LED_GPIO, LED_N);
}

void ws2812_set_brightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_bright = percent;
}

void ws2812_set(int i, uint8_t r, uint8_t g, uint8_t b) {
    if (i < 0 || i >= LED_N) return;
    s_r[i] = r; s_g[i] = g; s_b[i] = b;
}

void ws2812_clear(void) {
    memset(s_r, 0, sizeof(s_r));
    memset(s_g, 0, sizeof(s_g));
    memset(s_b, 0, sizeof(s_b));
}

static inline void put_bit(int *k, int one) {
    if (one) { s_sym[*k].level0 = 1; s_sym[*k].duration0 = T1H;
               s_sym[*k].level1 = 0; s_sym[*k].duration1 = T1L; }
    else     { s_sym[*k].level0 = 1; s_sym[*k].duration0 = T0H;
               s_sym[*k].level1 = 0; s_sym[*k].duration1 = T0L; }
    (*k)++;
}

void ws2812_show(void) {
    if (!s_ok) return;
    int k = 0;
    for (int i = 0; i < LED_N; i++) {
        uint8_t g = (uint8_t)((s_g[i] * s_bright) / 100);
        uint8_t r = (uint8_t)((s_r[i] * s_bright) / 100);
        uint8_t b = (uint8_t)((s_b[i] * s_bright) / 100);
        uint8_t bytes[3] = {g, r, b};   /* WS2812 = GRB */
        for (int by = 0; by < 3; by++)
            for (int bit = 7; bit >= 0; bit--)
                put_bit(&k, (bytes[by] >> bit) & 1);
    }
    rmt_transmit_config_t tc = { .loop_count = 0 };
    if (rmt_transmit(s_tx, s_enc, s_sym, sizeof(s_sym), &tc) == ESP_OK)
        rmt_tx_wait_all_done(s_tx, 100);
}
