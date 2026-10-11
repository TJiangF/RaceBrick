/* RaceBrick firmware — ESP32-S3 + ST7789 (360x240 landscape) pixel UI.
 * App shell lives in app.c / screens.c; this file is the main task loop.
 */
#include "config.h"
#include "gfx.h"
#include "lcd.h"
#include "input.h"
#include "app.h"

#include "extflash.h"
#include "gps.h"
#include "battery.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#if LCD_SELFTEST
#include <stdio.h>
static void selftest(void) {
    uint16_t RED = RGB565(255, 0, 0), GRN = RGB565(0, 255, 0), BLU = RGB565(0, 0, 255);
    uint16_t WHT = RGB565(255, 255, 255), YEL = RGB565(255, 255, 0), CYA = RGB565(0, 255, 255);
    int f = 0;
    for (;;) {
        gfx_clear(0);
        gfx_rect(0, 0, LCD_W, LCD_H, WHT);
        gfx_rect(1, 1, LCD_W - 2, LCD_H - 2, WHT);
        int hw = LCD_W / 2, hh = LCD_H / 2;
        gfx_fill(2, 2, hw - 3, hh - 3, RED);                 /* top-left  RED   */
        gfx_fill(hw + 1, 2, LCD_W - hw - 3, hh - 3, GRN);    /* top-right GREEN */
        gfx_fill(2, hh + 1, hw - 3, LCD_H - hh - 3, BLU);    /* bot-left  BLUE  */
        gfx_fill(hw + 1, hh + 1, LCD_W - hw - 3, LCD_H - hh - 3, WHT); /* bot-right WHITE */
        for (int x = 0; x < LCD_W; x += 40) gfx_vline(x, 0, LCD_H, CYA);
        gfx_fill(4, 4, 200, 34, 0);
        gfx_text(6, 6, "SELFTEST 320x240", WHT, 1);
        gfx_text(6, 20, "TL R / TR G / BL B / BR W", YEL, 1);
        int bx = (f * 3) % (LCD_W - 24);
        gfx_fill(bx, LCD_H - 24, 24, 20, YEL);
        lcd_flush(gfx_fb());
        f++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
#endif

void app_main(void) {
    lcd_init();
    gfx_init();
#if LCD_SELFTEST
    gfx_reset_view();
    selftest();
#endif
#if EXT_FLASH_TEST
    ESP_LOGI("main", "external flash self-test:");
    if (ext_flash_selftest()) ESP_LOGI("main", "EXT FLASH: PASS");
    else                      ESP_LOGI("main", "EXT FLASH: FAIL");
#endif
    /* GPS bring-up over UART (ESP RX=12, TX=11 @ 38400) */
    gps_begin();
    for (int i = 0; i < 40; i++) { gps_poll(); vTaskDelay(pdMS_TO_TICKS(20)); }

    battery_init();
    input_init();
    app_init();

    ESP_LOGI("racebrick", "RaceBrick UI up (%dx%d)", LCD_W, LCD_H);

    const float target_dt = 1.0f / 30.0f;
    int64_t last = esp_timer_get_time();
    int64_t last_gps_log = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();
        gps_poll();
        battery_update();
        if (now - last_gps_log >= 1000000) { last_gps_log = now; gps_dump(); }
        float dt = (float)(now - last) / 1e6f;
        last = now;
        if (dt < 0) dt = 0;
        if (dt > 0.1f) dt = 0.1f;

        input_tick(dt * 1000.0f);
#if BTN_ENABLE
        input_poll_hw();
#else
        app_autopilot(dt);
#endif

        event_t ev;
        while (input_pop(&ev)) app_dispatch(&ev);

        app_update(dt);
        app_render();
        lcd_flush(gfx_fb());

        int64_t spent = esp_timer_get_time() - now;
        int64_t frame_us = (int64_t)(target_dt * 1e6f);
        if (spent < frame_us) {
            vTaskDelay(pdMS_TO_TICKS((frame_us - spent + 999) / 1000));
        }
    }
}
