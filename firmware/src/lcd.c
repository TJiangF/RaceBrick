/* ST7789 SPI driver for the RaceBrick panel.
 *
 * The panel is physically 240x360 portrait; we drive it as a 360x240 landscape
 * framebuffer (see config.h). The framebuffer is RGB565 in native (little)
 * endianness and the panel is configured with LCD_RGB_DATA_ENDIAN_LITTLE so no
 * byte swapping is needed anywhere.
 */
#include "lcd.h"
#include "config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "lcd";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static SemaphoreHandle_t         s_flush_done;

#define BL_LEDC_MODE  LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHAN  LEDC_CHANNEL_0
#define BL_LEDC_RES   LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ  5000

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx) {
    (void)io; (void)edata;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &hp);
    return hp == pdTRUE;
}

/* backlight PWM */
static void bl_init(void) {
    ledc_timer_config_t tcfg = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));

    ledc_channel_config_t ccfg = {
        .gpio_num = LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHAN,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
}

void lcd_set_brightness(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)((percent * 1023) / 100);
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHAN, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHAN);
}

void lcd_init(void) {
    s_flush_done = xSemaphoreCreateBinary();

    /* ---- SPI bus ---- */
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_SCLK,
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* ---- panel IO ---- */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_flush_done,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                             &io_config, &s_io));

    /* ---- panel ---- */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, LCD_INVERT));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, LCD_SWAP_XY));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    if (LCD_GAP_X || LCD_GAP_Y) {
        ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y));
    }
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    bl_init();
    lcd_set_brightness(60);

    ESP_LOGI(TAG, "ST7789 ready  %dx%d", LCD_W, LCD_H);
}

void lcd_reconfigure(int swap_xy, int mirror_x, int mirror_y, int invert) {
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    esp_lcd_panel_invert_color(s_panel, invert);
}

void lcd_set_gap(int x, int y) {
    esp_lcd_panel_set_gap(s_panel, x, y);
}

void lcd_flush(const uint16_t *fb) {
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, fb);
    /* wait for the frame to finish so the caller may safely reuse fb */
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}
