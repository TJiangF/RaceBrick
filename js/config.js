/* RaceChrono Web Demo - global configuration.
 * All values here are shared 1:1 with the eventual ESP32-S3/LVGL firmware.
 */
(function () {
  const RC = (window.RC = window.RC || {});

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

    /* ---- palette ---- */
    COLORS: {
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
    },

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
  };
})();
