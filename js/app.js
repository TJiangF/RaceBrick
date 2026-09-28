/* Application shell: main loop, screen transitions, overlays, chrome, debug. */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;
  const D = RC.DOM;
  const TRANSITION_MS = 220;
  const HOME_PAGES = ['HOME', 'HISTORY', 'TRACK_EDIT', 'SETTINGS'];

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
      this.imu = new RC.Mpu6050();
      this.simSpeed = 1;
      this.tracks = this.seedTracks();
      this.raceLog = [];
      this.fusionProfile = 'chassis';
      this.raceMount = 'chassis';
      this.theme = 'dark';
      this.raceTarget = 'RACING';
      this.sessionTrack = null;
      this.sessionSections = [];
      this.sessionVehicle = 0;
      this.sessionLaps = [];
      this.logLines = [];
      this._debugT = 0;

      /* each power-on asks the user to level-calibrate the MPU6050 */
      /* seed a few demo race sessions so the HISTORY page is populated */
      this.seedHistory();

      RC.setTheme(this.theme);
      this.go('IMU_CALIB');

      this.last = performance.now();
      this.loop = this.loop.bind(this);
      requestAnimationFrame(this.loop);
    }

    seedTracks() {
      const t = RC.TRACK_DEFS.map((def) => new RC.Track(def));
      t[0].sections = [0.26, 0.55, 0.8];
      t[1].sections = [0.4, 0.72];
      t[2].sections = [0.33, 0.66];
      return t;
    }

    /* build a race record from an engine that has been running */
    makeRecord(engine, meta) {
      const laps = engine.laps.map((l) => ({
        index: l.index,
        time: l.time,
        times: l.times,
        trace: l.trace,
      }));
      return {
        date: meta.date,
        trackName: meta.trackName || engine.track.name,
        mode: meta.mode || 'chassis',
        duration: meta.duration != null ? meta.duration : engine.time,
        laps: laps,
        lapCount: laps.length,
        bestIndex: engine.best ? engine.best.index : null,
        bestTime: engine.best ? engine.best.time : null,
        sections: (engine.track.sections || []).slice(),
      };
    }

    recordRace(engine, mode) {
      if (!engine || engine._logged) return;
      engine._logged = true;
      if (!engine.laps.length) return;
      this.raceLog.push(
        this.makeRecord(engine, {
          date: this._stamp(),
          mode: mode,
        })
      );
    }

    _stamp() {
      const d = new Date();
      const p = (n) => String(n).padStart(2, '0');
      return p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
    }

    seedHistory() {
      const demo = [
        { track: 0, laps: 7, dur: 600, date: '09-27 18:20' },
        { track: 1, laps: 5, dur: 420, date: '09-27 17:05' },
        { track: 2, laps: 4, dur: 300, date: '09-26 20:41' },
      ];
      for (const d of demo) {
        const t = this.tracks[d.track];
        if (!t) continue;
        const e = new RC.RaceEngine(t, { sensor: this.imu });
        e.setProfile('chassis');
        e._logged = false;
        let guard = 0;
        while (e.laps.length < d.laps && guard++ < 400000) e.update(0.05);
        this.raceLog.push(
          this.makeRecord(e, {
            date: d.date,
            trackName: t.name,
            duration: d.dur,
          })
        );
      }
    }

    /* ---------------- navigation ---------------- */
    go(name, dir, opts) {
      dir = dir || 'left';
      const Cls = RC.SCREENS[name];
      if (!Cls) {
        console.warn('unknown screen', name);
        return;
      }
      const scr = new Cls(this, opts);
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

    setTheme(name) {
      this.theme = name === 'light' ? 'light' : 'dark';
      RC.setTheme(this.theme);
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
