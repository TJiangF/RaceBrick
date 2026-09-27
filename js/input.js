/* Input layer.
 *
 * The UI layer never knows where a button event came from. Sources:
 *   - keyboard (web sim)
 *   - on-screen buttons (web sim, mouse/touch)
 *   - GPIO polling (real board, via attachGPIO(source))
 *
 * Emitted events (identical on web and firmware):
 *   PRESS     short press  (< LONG_PRESS_MS)   -> on release
 *   LONG      held >= LONG_PRESS_MS
 *   REPEAT    continuous repeat for Up/Down after LONG
 *   RELEASED  physical release (always)
 */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;
  const BUTTONS = ['Up', 'Down', 'Push', 'Ok', 'Back'];
  const REPEATABLE = { Up: true, Down: true };

  class Input {
    constructor() {
      this.listeners = [];
      this.buttons = {};
      this.clock = 0; // ms monotonic (driven by app loop)
      BUTTONS.forEach((b) => {
        this.buttons[b] = { down: false, t0: 0, longFired: false, nextRepeat: 0 };
      });
      this.gpioSource = null;
      this.gpioState = {};
      this.simulate = null; // optional external state source
    }

    on(fn) {
      this.listeners.push(fn);
    }
    emit(type, button) {
      const ev = { type, button, t: this.clock };
      for (const f of this.listeners) f(ev);
    }

    press(button) {
      const b = this.buttons[button];
      if (!b || b.down) return;
      b.down = true;
      b.t0 = this.clock;
      b.longFired = false;
      b.nextRepeat = 0;
    }
    release(button) {
      const b = this.buttons[button];
      if (!b || !b.down) return;
      if (!b.longFired) this.emit('PRESS', button);
      b.down = false;
      this.emit('RELEASED', button);
    }
    cancel(button) {
      const b = this.buttons[button];
      if (b) b.down = false;
    }

    tick(dtMs) {
      this.clock += dtMs;
      for (const name in this.buttons) {
        const b = this.buttons[name];
        if (!b.down) continue;
        const held = this.clock - b.t0;
        if (!b.longFired && held >= C.LONG_PRESS_MS) {
          b.longFired = true;
          this.emit('LONG', name);
          if (REPEATABLE[name]) b.nextRepeat = this.clock + C.REPEAT_INTERVAL_MS;
        } else if (b.longFired && REPEATABLE[name]) {
          if (this.clock >= b.nextRepeat) {
            b.nextRepeat += C.REPEAT_INTERVAL_MS;
            this.emit('REPEAT', name);
          }
        }
      }
    }

    /* ---------- keyboard source ---------- */
    attachKeyboard(target) {
      const map = C.KEY_MAP;
      target.addEventListener('keydown', (e) => {
        const btn = map[e.key];
        if (!btn) return;
        e.preventDefault();
        if (!e.repeat) this.press(btn);
      });
      target.addEventListener('keyup', (e) => {
        const btn = map[e.key];
        if (!btn) return;
        e.preventDefault();
        this.release(btn);
      });
      target.addEventListener('blur', () => {
        for (const n in this.buttons) this.cancel(n);
      });
    }

    /* ---------- GPIO source (reserved for firmware) ----------
     * Pass an object exposing getButtonState() -> { Up:bool, Down:bool, ... }.
     * The GPIO ISR / poll driver just flips these booleans; the debounce and
     * long-press timing below stay identical to the web build.
     */
    attachGPIO(source) {
      this.gpioSource = source;
    }
    pollGPIO() {
      if (!this.gpioSource) return;
      const st = this.gpioSource.getButtonState() || {};
      for (const name of BUTTONS) {
        const want = !!st[name];
        const b = this.buttons[name];
        if (want && !b.down) this.press(name);
        else if (!want && b.down) this.release(name);
      }
    }
  }

  RC.Input = Input;
})();
