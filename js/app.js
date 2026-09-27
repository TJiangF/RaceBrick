/* Application shell: main loop, screen transitions, overlays, chrome, debug. */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;
  const D = RC.DOM;
  const TRANSITION_MS = 220;
  const HOME_PAGES = ['HOME', 'TRACK_EDIT', 'SETTINGS'];

  class App {
    constructor() {
      this.stage = document.getElementById('stage');
      this.statusEl = document.getElementById('statusBar');
      this.toastHost = document.getElementById('toastHost');

      /* LED diff bar */
      this.ledEls = [];
      const ledBar = document.getElementById('ledBar');
      for (let i = 0; i < C.LED_COUNT; i++) {
        const l = D.div('led off');
        ledBar.appendChild(l);
        this.ledEls.push(l);
      }

      /* input + event queue */
      this.input = new RC.Input();
      this.input.attachKeyboard(window);
      this.queue = [];
      this.input.on((ev) => this.queue.push(ev));

      /* state */
      this.current = null;
      this.overlay = null;
      this.stateName = '';
      this.homeIndex = 0;
      this.engine = null;
      this.gnss = new RC.Gnss();
      this.simSpeed = 1;
      this.tracks = this.seedTracks();
      this.sessionTrack = null;
      this.sessionSections = [];
      this.sessionVehicle = 0;
      this.sessionLaps = [];
      this.logLines = [];
      this._debugT = 0;

      this.go('HOME');

      this.last = performance.now();
      this.loop = this.loop.bind(this);
      requestAnimationFrame(this.loop);
    }

    seedTracks() {
      return RC.TRACK_DEFS.map((def) => new RC.Track(def));
    }

    /* ---------------- navigation ---------------- */
    go(name, dir) {
      dir = dir || 'left';
      const Cls = RC.SCREENS[name];
      if (!Cls) {
        console.warn('unknown screen', name);
        return;
      }
      const scr = new Cls(this);
      const container = D.div('screen');
      scr.mount(container);
      scr._container = container;
      scr._name = name;

      const old = this.current;
      this.current = scr;
      this.stateName = name;

      this.stage.appendChild(container);
      /* force a layout flush so the incoming frame is fully painted before the
       * animation starts (prevents ghosting / stale buffer) */
      const startTx =
        dir === 'left' ? 'translateX(100%)' : dir === 'right' ? 'translateX(-100%)' : 'translateY(100%)';
      container.style.transform = startTx;
      void container.offsetWidth;

      container.style.transition = 'transform ' + TRANSITION_MS + 'ms ease-out';
      container.style.transform = 'translateX(0) translateY(0)';
      if (old) {
        old._container.style.transition = 'transform ' + TRANSITION_MS + 'ms ease-out, opacity ' +
          TRANSITION_MS + 'ms ease-out';
        if (dir === 'left') old._container.style.transform = 'translateX(-100%)';
        else if (dir === 'right') old._container.style.transform = 'translateX(100%)';
        else {
          old._container.style.transform = 'translateY(-12%)';
          old._container.style.opacity = '0.35';
        }
        setTimeout(() => {
          if (old._container && old._container.parentNode) old._container.remove();
          if (old.onUnmount) old.onUnmount();
        }, TRANSITION_MS + 20);
      }
    }

    goHome(delta) {
      const n = HOME_PAGES.length;
      this.homeIndex = ((this.homeIndex + delta) % n + n) % n;
      this.go(HOME_PAGES[this.homeIndex], delta > 0 ? 'left' : 'right');
    }
    goHomeTo(i) {
      const dir = i < this.homeIndex ? 'right' : 'left';
      this.homeIndex = i;
      this.go(HOME_PAGES[i], dir);
    }

    /* ---------------- overlays ---------------- */
    pushOverlay(name, opts) {
      if (this.overlay) this.popOverlay('down');
      const Cls = RC.SCREENS[name];
      const scr = new Cls(this, opts);
      const container = D.div('overlay');
      container.appendChild(D.div('overlay-backdrop'));
      scr.mount(container);
      scr._container = container;
      container.style.opacity = '0';
      this.stage.appendChild(container);

      const card = scr.root;
      card.style.transform = 'translateY(120%)';
      void container.offsetWidth; /* flush */
      container.style.transition = 'opacity 160ms ease-out';
      container.style.opacity = '1';
      card.style.transition = 'transform ' + TRANSITION_MS + 'ms cubic-bezier(.2,.8,.2,1)';
      card.style.transform = 'translateY(0)';
      this.overlay = scr;
    }
    popOverlay() {
      if (!this.overlay) return;
      const scr = this.overlay;
      this.overlay = null;
      const container = scr._container;
      const card = scr.root;
      card.style.transition = 'transform 170ms ease-in';
      card.style.transform = 'translateY(120%)';
      container.style.transition = 'opacity 170ms ease-in';
      container.style.opacity = '0';
      setTimeout(() => container.remove(), 190);
    }

    toast(msg, ms) {
      const el = D.div('toast', msg);
      this.toastHost.appendChild(el);
      void el.offsetWidth;
      el.classList.add('show');
      setTimeout(() => {
        el.classList.remove('show');
        setTimeout(() => el.remove(), 220);
      }, ms || 1400);
    }

    /* ---------------- event dispatch ---------------- */
    dispatch(ev) {
      this.log(ev);
      if (this.overlay) this.overlay.handle(ev);
      else if (this.current) this.current.handle(ev);
    }

    log(ev) {
      this.logLines.push(ev.button + ' · ' + ev.type);
      if (this.logLines.length > 40) this.logLines.shift();
    }

    /* ---------------- frame ---------------- */
    loop(ts) {
      const dtMs = Math.min(64, ts - this.last);
      this.last = ts;
      const dt = dtMs / 1000;

      this.input.tick(dtMs);
      this.input.pollGPIO();
      if (this.gnss) this.gnss.update(dt);
      while (this.queue.length) this.dispatch(this.queue.shift());

      if (this.current && this.current.update) this.current.update(dt);
      if (this.overlay && this.overlay.update) this.overlay.update(dt);

      this.renderChrome();
      this.updateDebug(dtMs);

      requestAnimationFrame(this.loop);
    }

    renderChrome() {
      let ch = {};
      if (this.current && this.current.getChrome) ch = this.current.getChrome() || {};
      const leds = ch.leds;
      for (let i = 0; i < C.LED_COUNT; i++) {
        const cls = 'led ' + (leds && leds[i] ? leds[i] : 'off');
        if (this.ledEls[i].className !== cls) this.ledEls[i].className = cls;
      }
      this.statusEl.textContent = ch.status || '';
      this.statusEl.style.color = ch.statusColor || C.COLORS.dim;
    }

    updateDebug(dtMs) {
      this._debugT += dtMs;
      if (this._debugT < 100) return;
      this._debugT = 0;
      const e = this.engine;
      const held = Object.keys(this.input.buttons)
        .filter((k) => this.input.buttons[k].down)
        .join(',');
      const dl = document.getElementById('dbgState');
      if (dl) dl.textContent = this.stateName + (this.overlay ? ' +MODAL' : '');
      const dl2 = document.getElementById('dbgHeld');
      if (dl2) dl2.textContent = held || '-';
      const dl3 = document.getElementById('dbgRace');
      if (dl3) {
        dl3.textContent = e
          ? 'lap ' + e.lap + '  t=' + e.elapsed.toFixed(2) + '  best=' +
            (e.best ? e.best.time.toFixed(3) : '--') + '  diff=' + e.diff.toFixed(3)
          : '-';
      }
      const lg = document.getElementById('dbgLog');
      if (lg) {
        lg.innerHTML = '';
        this.logLines
          .slice(-8)
          .reverse()
          .forEach((l) => lg.appendChild(D.div('', l)));
      }
    }

    /* ---------------- debug controls ---------------- */
    buttonDown(id) {
      this.input.press(id);
    }
    buttonUp(id) {
      this.input.release(id);
    }
    setSimSpeed(x) {
      this.simSpeed = x;
      const el = document.getElementById('dbgSpeed');
      if (el) el.textContent = x + 'x';
    }
    injectLap() {
      if (this.engine) {
        this.engine.s = this.engine.N - 0.001;
      }
    }
  }

  RC.App = App;
  window.addEventListener('DOMContentLoaded', () => {
    RC.app = new App();
  });
})();
