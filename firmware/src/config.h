/* RaceBrick firmware configuration.
 * Pins, panel geometry and input timing. Timings must match the web demo
 * (https://github.com/TJiangF/RaceBrick, js/config.js).
 */
#pragma once

/* ---------------- ST7789 wiring (new PCB) ----------------
 * BL-io38  DC-io39  MOSI-io40  CS-io41  SCK-io42 (MISO is not wired) */
#define LCD_MOSI   40
#define LCD_SCLK   42
#define LCD_CS     41
#define LCD_DC     39
#define LCD_RST    (-1)   /* not wired; software reset only */
#define LCD_BL     38

#define LCD_HOST    SPI2_HOST
#define LCD_PCLK_HZ (40 * 1000 * 1000)

/* 1 = boot straight into a color/geometry test pattern instead of the UI */
#define LCD_SELFTEST 0
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8

/* ---------------- panel geometry (LANDSCAPE logical 320x240) ----------------
 * Verified on the bench: the module's controller is a standard ST7789S with a
 * 240x320 GRAM (native portrait 240x320 -> landscape 320x240). */
#define LCD_W   320
#define LCD_H   240
/* no top/bottom chrome any more: pages own the full panel */
#define CONTENT_Y 0
#define CONTENT_H LCD_H                       /* 240 */

/* 1 = boot into the daylight theme (Settings > THEME toggles at runtime) */
#define START_THEME_LIGHT 0

/* boot backlight level, 0..100 (0 = backlight off) */
#define START_BRIGHTNESS 60

/* WS2812 LED strip: DI on GPIO48, first LED = leftmost. 10 LEDs. */
#define LED_GPIO 48
#define START_LED_BRIGHT 60   /* global LED brightness 0..100 */

/* buzzer (PIN_BEE) global volume 0..100 at boot */
#define START_SOUND_VOL 0

/* panel mounting / rotation — verified on the bench: CFG4 = S1 X1 Y0 I0 */
#define LCD_SWAP_XY  1
#define LCD_MIRROR_X 1
#define LCD_MIRROR_Y 0
#define LCD_INVERT   0
/* some 240x360 panels need a small offset (in native portrait coords) */
#define LCD_GAP_X    0
#define LCD_GAP_Y    0

/* ---------------- input: RKJXM1015004 5-way + back, V4.0 ----------------
 * Up-io15  Down-io18  Left-io17  Right-io8  Push-io16  Back-io5
 * The stick common pulls PUSH low for every direction. Direction events are
 * decoded from the direction pins; PUSH is emitted only when no direction is
 * active (true center press). Hardware RC debounce is fitted on V4.0, so the
 * firmware performs edge detection only, with no software debounce delay. */
#define BTN_ENABLE   1
#define PIN_BTN_UP    16
#define PIN_BTN_DOWN 17
#define PIN_BTN_LEFT 7
#define PIN_BTN_RIGHT 18
#define PIN_BTN_PUSH 15
#define PIN_BTN_BACK  4
#define BTN_ACTIVE_LOW 1
#define PIN_ENC_A     8
#define PIN_ENC_B    19

/* Keep the RaceBrick-XXXX SoftAP available even without saved home WiFi, so
 * the phone can connect directly for track/session sync. */
#define WIFI_AP_ALWAYS 1
/* 0.25 dBm units: 34 = 8.5 dBm. Lower peak current helps USB/battery rails. */
#define WIFI_AP_TX_POWER_QDBM 34

/* ---------------- external SPI flash (W25Q128), Rev.C ----------------
 * FLASH-CS-io41  FLASH-MISO-io40  FLASH-SCK-io39  FLASH-MOSI-io38
 * uses SPI3 (the LCD stays on SPI2) */
#define EXT_FLASH_CS    13
#define EXT_FLASH_MISO  14
#define EXT_FLASH_SCLK  12
#define EXT_FLASH_MOSI  11
#define EXT_FLASH_HZ    (10 * 1000 * 1000)
#define EXT_FLASH_TEST  1     /* boot probe: JEDEC only, non-destructive */

/* ---------------- other on-board peripherals (new PCB) --------------------
 * I2C (MPU6050) Scl-io6  Sda-io5   (MPU INT not wired)
 * GNSS NEO-M9N  ESP TX-io9  ESP RX-io10  TP-io20
 * Buzzer BEE-io21  WS2812 DI-io48
 * VBAT sense switch/check io47 (drive HIGH to sample)  adc-io1
 * Encoder A-io8  B-io19  (quadrature, raw state exposed in BUTTON DEBUG)
 */
#define PIN_I2C_SCL 6
#define PIN_I2C_SDA 5
#define PIN_MPU_INT (-1)  /* not wired on the new PCB */
#define PIN_GPS_TX  9    /* ESP TX -> NEO-RX (module RX) */
#define PIN_GPS_RX  10   /* ESP RX <- NEO-TX (module TX) */
#define PIN_GPS_TP  20
#define PIN_BEE     21
#define PIN_WS2812  48
#define PIN_VBAT_EN 47   /* Rev.C: io21 is now LCD MOSI */
#define PIN_VBAT_ADC 1

/* ---------------- MPU6050 axis correction ----------------
 * The PCB is mounted vertically and the chip silkscreen is reversed (head
 * down), so forward/back and left/right read inverted. Adjust signs/swap here
 * after testing so the G axes match the car. */
#define IMU_SIGN_X (-1)
#define IMU_SIGN_Y (-1)
#define IMU_SIGN_Z ( 1)
#define IMU_SWAP_XY 0
#define IMU_SWAP_XZ 0

/* IMU debug bubble mapping (tuned on the bench): which signed axis is
 * "forward" (+ = dot up) and "left" (+ = dot left). If the car mounting
 * makes the dot disagree, adjust these. */
/* Inertial ("seat") feel: accelerating forward throws the ball back (down),
 * turning left throws it right. */
#define IMU_BUB_FWD_AXIS 2     /* Z = forward/back */
#define IMU_BUB_FWD_SIGN (-1)  /* forward -> dot down (backward) */
#define IMU_BUB_LAT_AXIS 0     /* X = left/right */
#define IMU_BUB_LAT_SIGN (-1)  /* left -> dot right */

/* Racing G-meter axes (G-G plot: right positive, forward positive). */
#define IMU_GM_LAT_AXIS  0     /* X = lateral */
#define IMU_GM_LAT_SIGN  (-1)  /* right = + */
#define IMU_GM_LONG_AXIS 2     /* Z = longitudinal */
#define IMU_GM_LONG_SIGN ( 1)  /* accelerating = + */

/* ---------------- interaction timing (ms), identical to the web demo ------- */
#define LONG_PRESS_MS   600
#define REPEAT_MS       110
#define DOUBLE_MS       260   /* two PUSH presses within this window = EV_DOUBLE */

/* ---------------- simulation (mirrors RC.CONFIG) ---------------- */
#define TRK_N           360       /* samples per lap */
#define SIM_SPEED       1.0f      /* time multiplier */
#define LED_COUNT       10
#define DIFF_PER_LED    0.5f
#define MAX_LED         5
#define LED_DELTA_WINDOW_M 30.0f
#define LED_SEG_PER_LED    0.10f
#define MAX_LAPS        8
#define TRACE_MAX       160       /* fused points kept per lap */
#define GPS_MAX         80        /* raw GPS points kept per lap */
#define MAX_SECTIONS    8
#define MAX_TRACKS      16
#define HISTORY_RECORDS 32
#define HISTORY_LAPS    16
#define REC_INITIAL     4096  /* PSRAM-backed recording buffer, grows as needed */
#define REC_FUSION_LAPS 16    /* completed laps kept for average-track fusion */

/* performance test: default launch threshold in 0.01 g (Settings > LAUNCH G) */
#define START_PERF_TRIG 15
#define PERF_BRAKE_G    0.25f

/* MPU6050 + GPS/IMU fusion (RC.CONFIG.IMU) */
#define TRACK_WIDTH_M  5.0f   /* default modelled track width, metres */
#define START_LINE_WIDTH_M 10.0f
#define START_LINE_HALF_M  (START_LINE_WIDTH_M * 0.5f)
#define IMU_CALIB_MS        1500.0f
#define IMU_STABLE_TILT     0.025f
#define IMU_STABLE_GYRO     3.0f
#define IMU_STABLE_WIN      20
#define IMU_STABLE_PEAK_A   0.15f
#define IMU_STABLE_PEAK_G   15.0f
#define IMU_ACC_NOISE   0.012f
#define IMU_GYRO_NOISE  0.35f
#define GPS_NOISE       0.017f
#define GPS_HZ          8.0f
#define GPS_DROPOUT     0.06f
#define FUSION_ALPHA_CHASSIS  0.45f
#define FUSION_BETA_CHASSIS   0.12f
#define FUSION_ACC_CHASSIS    0.8f
#define FUSION_ALPHA_STEER    0.85f
#define FUSION_BETA_STEER     0.35f
#define FUSION_ACC_STEER      0.05f
