#include "buzzer.h"
#include "config.h"
#include "driver/ledc.h"
#include "esp_timer.h"

/* LEDC timer/channel 1 (the backlight uses timer/channel 0). */
#define BEE_MODE  LEDC_LOW_SPEED_MODE
#define BEE_TIMER LEDC_TIMER_1
#define BEE_CHAN  LEDC_CHANNEL_1
#define BEE_RES   LEDC_TIMER_10_BIT

void buzzer_init(void) {
    ledc_timer_config_t t = {
        .speed_mode = BEE_MODE,
        .duty_resolution = BEE_RES,
        .timer_num = BEE_TIMER,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);

    ledc_channel_config_t c = {
        .gpio_num = PIN_BEE,
        .speed_mode = BEE_MODE,
        .channel = BEE_CHAN,
        .timer_sel = BEE_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&c);
}

void buzzer_tone(int freq_hz, int volume_percent) {
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    if (freq_hz > 0) ledc_set_freq(BEE_MODE, BEE_TIMER, freq_hz);
    /* passive buzzer: PWM frequency = pitch, duty = loudness. A square
     * wave is loudest near 50% duty, so map 1..100% -> ~5%..50%. */
    uint32_t duty = 0;
    if (volume_percent > 0)
        duty = 51 + (uint32_t)(volume_percent * (512 - 51)) / 100;
    ledc_set_duty(BEE_MODE, BEE_CHAN, duty);
    ledc_update_duty(BEE_MODE, BEE_CHAN);
}

void buzzer_off(void) {
    ledc_set_duty(BEE_MODE, BEE_CHAN, 0);
    ledc_update_duty(BEE_MODE, BEE_CHAN);
}

/* ---- non-blocking melody sequencer ---- */
static int  s_vol = 60;              /* global volume 0..100 */
static const note_t *s_seq = 0;
static int  s_n = 0, s_i = 0;
static int64_t s_next = 0;
static bool s_active = false;

void buzzer_set_volume(int volume_percent) {
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    s_vol = volume_percent;
    if (s_vol == 0) buzzer_off();
}

void buzzer_play(const note_t *seq, int n) {
    if (n <= 0 || !s_vol) return;
    s_seq = seq; s_n = n; s_i = 0; s_active = true;
    if (seq[0].freq == 0) buzzer_off(); else buzzer_tone(seq[0].freq, s_vol);
    s_next = esp_timer_get_time() + (int64_t)seq[0].ms * 1000;
}

void buzzer_update(void) {
    if (!s_active) return;
    int64_t now = esp_timer_get_time();
    if (now < s_next) return;
    s_i++;
    if (s_i >= s_n) { s_active = false; buzzer_off(); return; }
    if (s_seq[s_i].freq == 0) buzzer_off(); else buzzer_tone(s_seq[s_i].freq, s_vol);
    s_next = now + (int64_t)s_seq[s_i].ms * 1000;
}

void buzzer_play_id(int id) {
    static const note_t confirm[] = {{880, 70}, {1175, 130}};                    /* rising prompt */
    static const note_t calib[]   = {{523, 90}, {659, 90}, {784, 90}, {1047, 170}}; /* C-E-G-C */
    static const note_t fix[]     = {{784, 80}, {988, 80}, {1319, 180}};         /* G-B-E */
    switch (id) {
    case SND_CONFIRM: buzzer_play(confirm, 2); break;
    case SND_CALIB:   buzzer_play(calib, 4); break;
    case SND_FIX:     buzzer_play(fix, 3); break;
    }
}
