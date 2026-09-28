/* RaceChrono Web Demo - global configuration.
 * All values here are shared 1:1 with the eventual ESP32-S3/LVGL firmware.
 */
(function () {
  const RC = (window.RC = window.RC || {});

  /* ===================== themes ===================== */
  const DARK = {
    bg: '#0b0b0d',
    stage: '#0e0e12',
    panel: '#141418',
    line: '#2b2b33',
    text: '#d7d7dc',
    dim: '#767b84',
    white: '#f2f2f5',
    cyan: '#66e0d2',
    green: '#44ff44',
    red: '#ff4444',
    ledOff: '#222222',
    purple: '#8b5cf6',
    gold: '#ffd700',
    yellow: '#ffff44',
    orange: '#f5a97f',
    deviceBg: '#000000',
    statusBg: '#050506',
    surface: '#131318',
    surface2: '#1a1a22',
    line2: '#23232b',
    selTint: '#16262a',
    liveBg: '#10202a',
    text2: '#b7bcc4',
    glyphOff: '#4a4a52',
    dotOff: '#33333c',
    checkboxOff: '#55555f',
    goldTint: '#2a2408',
    bestBorder: '#b28cff',
    prn: '#8a8f97',
    mapBg: '#0e0e12',
    mapGrid: '#15151d',
    mapBorder: '#1b1b24',
    centerline: '#1e1e29',
    histTrace: '#2c3a3f',
    curTrace: '#66e0d2',
    gpsTrace: '#4a3540',
    gpsTraceHist: '#3a2e33',
    gmRing: '#20202a',
    gmTrail: '#2b4a44',
    gmCenter: '#2b2b33',
  };

  /* daylight: light surfaces, dark text, deeper accents for readability */
  const LIGHT = {
    bg: '#eef1f4',
    stage: '#f7f8fa',
    panel: '#ffffff',
    line: '#c6ccd6',
    text: '#23262c',
    dim: '#6b7280',
    white: '#111318',
    cyan: '#0a9e90',
    green: '#0f9d3a',
    red: '#d92b2b',
    ledOff: '#d4d8de',
    purple: '#6d3fd6',
    gold: '#a97900',
    yellow: '#b07a00',
    orange: '#c2611f',
    deviceBg: '#dfe3e8',
    statusBg: '#e6eaee',
    surface: '#f0f2f5',
    surface2: '#e3e6ea',
    line2: '#d5dae1',
    selTint: '#dff5f1',
    liveBg: '#e3f4f8',
    text2: '#3a3f47',
    glyphOff: '#b3b8c0',
    dotOff: '#c6ccd6',
    checkboxOff: '#9aa1ac',
    goldTint: '#fbf3d6',
    bestBorder: '#8f6be0',
    prn: '#6b7280',
    mapBg: '#f7f8fa',
    mapGrid: '#e2e5ea',
    mapBorder: '#d0d4db',
    centerline: '#d7dbe1',
    histTrace: '#b9c0ca',
    curTrace: '#0a9e90',
    gpsTrace: '#d8c4ba',
    gpsTraceHist: '#e4d5cd',
    gmRing: '#d7dbe1',
    gmTrail: '#8fbfb7',
    gmCenter: '#9aa0a8',
  };

  RC.THEMES = { dark: DARK, light: LIGHT };

  /* apply a theme: mutate COLORS in place (all `C.COLORS.x` update) and
   * toggle the CSS class that switches the DOM palette. */
  RC.setTheme = function (name) {
    const p = RC.THEMES[name] || RC.THEMES.dark;
    const COL = RC.CONFIG.COLORS;
    for (const k in p) COL[k] = p[k];
    RC.CONFIG.THEME = name;
    const dev = document.getElementById('device');
    if (dev) dev.classList.toggle('theme-light', name === 'light');
  };

  RC.CONFIG = {
    /* ---- screen geometry (physical pixels, LANDSCAPE) ---- */
    SCREEN_W: 360,
    SCREEN_H: 240,
    LED_H: 18,
    STATUS_H: 22,
    /* content area = 360 x 200 */

    /* ---- input timing (ms) : MUST match firmware ---- */
    LONG_PRESS_MS: 600,
    REPEAT_INTERVAL_MS: 110,
    DEBOUNCE_MS: 20,

    /* ---- lap diff bar ---- */
    LED_COUNT: 10,
    DIFF_PER_LED: 0.5, // seconds per LED
    MAX_LED: 5, // per side

    /* ---- active palette (dark by default; RC.setTheme swaps it) ---- */
    THEME: 'dark',
    COLORS: Object.assign({}, DARK),

    /* ---- keyboard -> logical button map (web only) ---- */
    KEY_MAP: {
      ArrowUp: 'Up',
      ArrowDown: 'Down',
      ArrowLeft: 'Up',
      ArrowRight: 'Down',
      Enter: 'Ok',
      ' ': 'Push',
      Spacebar: 'Push',
      Escape: 'Back',
      w: 'Up',
      s: 'Down',
      // debug: long-press helpers are simulated by holding the key
    },

    /* ---- reserved GPIO map for the real board ---- */
    GPIO: {
      up: 12,
      down: 13,
      push: 14,
      ok: 27,
      back: 26,
      led_start: 32,
      led_end: 39,
    },

    VEHICLES: [
      { id: 1, name: '卡丁车', color: '#ff6b6b' },
      { id: 2, name: '四冲车', color: '#ffd166' },
      { id: 3, name: '卡丁车+', color: '#66e0d2' },
      { id: 4, name: '赛车', color: '#8b5cf6' },
    ],

    LETTERS: 'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
    SIM_SPEEDS: [0.5, 1, 2, 4],

    /* ---- fusion weighting by mounting ----
     * chassis: IMU is rigidly fixed to the kart -> trust it (model-heavy)
     * steering: IMU turns with the wheel / is not rigid -> trust GPS, IMU lightly
     */
    FUSION_PROFILES: {
      chassis: { alpha: 0.45, beta: 0.12, accelWeight: 0.8 },
      steering: { alpha: 0.85, beta: 0.35, accelWeight: 0.05 },
    },

    /* ---- MPU6050 + GPS/IMU fusion ---- */
    IMU: {
      CALIB_MS: 2500, // required stable time during level calibration
      STABLE_TILT: 0.02, // g, max accel std to consider "flat / still"
      STABLE_GYRO: 2.5, // deg/s, max gyro std
      ACC_NOISE: 0.012, // g
      GYRO_NOISE: 0.35, // deg/s
      GPS_NOISE: 0.017, // normalized map units (~3 m fix)
      GPS_HZ: 8, // fix rate
      GPS_DROPOUT: 0.06, // fraction of fixes dropped (multipath/outage)
    },
  };
})();
