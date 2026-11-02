/* RaceBrick firmware configuration.
 * Pins, panel geometry and input timing. Timings must match the web demo
 * (https://github.com/TJiangF/RaceBrick, js/config.js).
 */
#pragma once

/* ---------------- ST7789 wiring (production board Rev.C) ----------------
 * CS-io12  SCK-io13  MISO-io14(unused)  MOSI-io21  DC-io10  BL-io11 */
#define LCD_MOSI   21
#define LCD_SCLK   13
#define LCD_CS     12
#define LCD_DC     10
#define LCD_RST    (-1)   /* not wired; software reset only */
#define LCD_BL     11

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

/* ---------------- input: 5-way + separate back, Rev.C ----------------
 * Up-io8  Down-io17  Left-io15  Right-io18  Push-io16  Back-io5
 * Set BTN_ENABLE to 1 to use the real buttons.
 * With BTN_ENABLE 0 the firmware runs a scripted autoplay demo. */
#define BTN_ENABLE   1
#define PIN_BTN_UP    8
#define PIN_BTN_DOWN 17
#define PIN_BTN_LEFT 15
#define PIN_BTN_RIGHT 18
#define PIN_BTN_PUSH 16
#define PIN_BTN_BACK  5
#define BTN_ACTIVE_LOW 1
#define DEBOUNCE_MS  20

/* ---------------- external SPI flash (W25Q128), Rev.C ----------------
 * FLASH-CS-io41  FLASH-MISO-io40  FLASH-SCK-io39  FLASH-MOSI-io38
 * uses SPI3 (the LCD stays on SPI2) */
#define EXT_FLASH_CS    41
#define EXT_FLASH_MISO  40
#define EXT_FLASH_SCLK  39
#define EXT_FLASH_MOSI  38
#define EXT_FLASH_HZ    (10 * 1000 * 1000)
#define EXT_FLASH_TEST  1     /* boot probe: JEDEC only, non-destructive */

/* ---------------- other on-board peripherals (Rev.C) -----------------------
 * I2C (MPU6050) Scl-io7  Sda-io6   (MPU6050 INT io4)
 * GNSS NEO-M9N  NEO-RX-io19 (=ESP TX)  NEO-TX-io20 (=ESP RX)
 * Buzzer BEE-io42   WS2812 DI-io48
 * VBAT sense  switch-io21 (drive HIGH to sample)  adc-io1  (reading x2)
 *
 * NEO-M9N_TP is io9 (free now that LCD DC moved to io10). */
#define PIN_I2C_SCL 7
#define PIN_I2C_SDA 6
#define PIN_MPU_INT 4
#define PIN_GPS_TX  19   /* ESP TX -> NEO-RX (module RX) */
#define PIN_GPS_RX  20   /* ESP RX <- NEO-TX (module TX) */
#define PIN_GPS_TP  9
#define PIN_BEE     42
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
#define MAX_LAPS        8
#define TRACE_MAX       160       /* fused points kept per lap */
#define GPS_MAX         80        /* raw GPS points kept per lap */
#define MAX_SECTIONS    8
#define MAX_TRACKS      6
#define HISTORY_RECORDS 5
#define HISTORY_LAPS    16
#define REC_MAX         400   /* max points while recording a real track (~800 m) */

/* performance test: default launch threshold in 0.01 g (Settings > LAUNCH G) */
#define START_PERF_TRIG 15
#define PERF_BRAKE_G    0.25f

/* MPU6050 + GPS/IMU fusion (RC.CONFIG.IMU) */
#define IMU_CALIB_MS    2500.0f
#define IMU_STABLE_TILT 0.02f
#define IMU_STABLE_GYRO 2.5f
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
