/* All UI screens + the state machine handlers.
 * Each screen builds its DOM once and mutates only what it needs each frame.
 */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;
  const D = RC.DOM;

  /* ---------- shared helpers ---------- */
  function pxbox(extra) {
    return D.div('pxbox' + (extra ? ' ' + extra : ''));
  }
  function title(t) {
    return D.div('scr-title', t);
  }
  function hint(t) {
    return D.div('scr-hint', t);
  }
  /* nav direction from an input event: -1 up, +1 down, 0 none */
  function navDir(ev) {
    if (ev.type !== 'PRESS' && ev.type !== 'LONG' && ev.type !== 'REPEAT') return 0;
    if (ev.button === 'Up') return -1;
    if (ev.button === 'Down') return 1;
    return 0;
  }
  /* windowed list slice centered on selection */
  function windowed(len, sel, K) {
    if (len <= K) return { start: 0, end: len };
    let start = sel - Math.floor(K / 2);
    start = Math.max(0, Math.min(start, len - K));
    return { start, end: start + K };
  }

  class Screen {
    constructor(app, opts) {
      this.app = app;
      this.opts = opts || {};
      this.root = null;
      this.chrome = { status: '', statusColor: '' };
      this.modal = false;
      this._t = 0;
    }
    build() {
      return D.div('screen-inner');
    }
    onMount() {}
    update(dt) {
      this._t += dt;
    }
    handle() {}
    onUnmount() {}
    mount(host) {
      this.host = host;
      this.root = this.build();
      host.appendChild(this.root);
      this.onMount();
    }
    getChrome() {
      return this.chrome;
    }
    setStatus(s, c) {
      this.chrome.status = s;
      if (c) this.chrome.statusColor = c;
    }
  }

  /* ================= HOME ================= */
  class HomeScreen extends Screen {
    build() {
      const r = D.div('screen-inner home');
      r.appendChild(D.div('home-logo', 'RACE'));
      r.appendChild(D.div('home-logo2', 'CHRONO'));
      r.appendChild(D.div('home-cta', 'START RACING'));
      const dots = D.div('dots');
      for (let i = 0; i < 3; i++) dots.appendChild(D.div('dot' + (i === 0 ? ' on' : '')));
      r.appendChild(dots);
      r.appendChild(hint('UP/DOWN  切换功能页'));
      r.appendChild(hint('OK  开始'));
      return r;
    }
    handle(ev) {
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok') this.app.go('MODE_SELECT', 'left');
      else if (ev.button === 'Down') this.app.goHome(1);
      else if (ev.button === 'Up') this.app.goHome(-1);
    }
    getChrome() {
      this.setStatus('HOME · START RACING');
      return this.chrome;
    }
  }

  /* ================= TRACK EDIT (home page 2) ================= */
  class TrackEditScreen extends Screen {
    build() {
      this.sel = 0;
      const r = D.div('screen-inner');
      this.entered = false;
      r.appendChild(title('TRACK EDIT'));
      this.sub = hint('需连接手机 App 修改');
      r.appendChild(this.sub);
      this.list = D.div('list');
      r.appendChild(this.list);
      this.footHint = hint('');
      r.appendChild(this.footHint);
      this.render();
      return r;
    }
    render() {
      this.list.innerHTML = '';
      this.app.tracks.forEach((t, i) => {
        const on = this.entered && i === this.sel;
        const b = pxbox('edit-row' + (on ? ' sel' : ''));
        b.appendChild(D.div('row-name', t.name));
        b.appendChild(D.div('row-sub', C.VEHICLES[t.vehicle].name));
        this.list.appendChild(b);
      });
      if (!this.app.tracks.length) this.list.appendChild(hint('（暂无赛道）'));
      this.footHint.textContent = this.entered
        ? 'UP/DOWN 选择 · OK 查看 · BACK 退出'
        : 'UP/DOWN 换页 · OK 进入编辑';
    }
    handle(ev) {
      if (!this.entered) {
        /* page-switch mode: Up/Down move between HOME / TRACK EDIT / SETTINGS */
        const d = navDir(ev);
        if (d) {
          this.app.goHome(d);
          return;
        }
        if (ev.type !== 'PRESS') return;
        if (ev.button === 'Ok') {
          this.entered = true;
          this.render();
        } else if (ev.button === 'Back') {
          this.app.goHomeTo(0);
        }
        return;
      }
      /* entered mode: Up/Down select a track */
      const d = navDir(ev);
      if (d && this.app.tracks.length) {
        this.sel = (this.sel + d + this.app.tracks.length) % this.app.tracks.length;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok' || ev.button === 'Push') {
        this.app.toast('需连接手机 App 修改');
      } else if (ev.button === 'Back') {
        this.entered = false;
        this.render();
      }
    }
    getChrome() {
      this.setStatus(this.entered ? 'TRACK EDIT · 编辑中' : 'TRACK EDIT · OK 进入');
      return this.chrome;
    }
  }

  /* ================= SETTINGS (home page 3) ================= */
  class SettingsScreen extends Screen {
    build() {
      this.sel = 0;
      this.entered = false;
      this.ledColors = ['彩色', '白色', '红色', '绿色'];
      this.settings = { brightness: 60, ledColor: 0, wifi: false, bluetooth: false };
      const r = D.div('screen-inner');
      r.appendChild(title('SETTINGS'));
      this.list = D.div('list');
      r.appendChild(this.list);
      this.footHint = hint('');
      r.appendChild(this.footHint);
      this.render();
      return r;
    }
    rowsData() {
      const s = this.settings;
      return [
        ['亮度', s.brightness + '%'],
        ['亮灯颜色', this.ledColors[s.ledColor]],
        ['WiFi', s.wifi ? '开' : '关'],
        ['蓝牙', s.bluetooth ? '开' : '关'],
        ['IMU 水平校准', '>'],
        ['GNSS 雷达测试', '>'],
      ];
    }
    render() {
      this.list.innerHTML = '';
      this.rowsData().forEach((it, i) => {
        const on = this.entered && i === this.sel;
        const b = pxbox('set-row' + (on ? ' sel' : ''));
        b.appendChild(D.div('set-k', it[0]));
        b.appendChild(D.div('set-v', it[1]));
        this.list.appendChild(b);
      });
      this.footHint.textContent = this.entered
        ? 'UP/DOWN 选择 · PUSH 调整 · BACK 退出'
        : 'UP/DOWN 换页 · OK 进入设置';
    }
    handle(ev) {
      if (!this.entered) {
        const d = navDir(ev);
        if (d) {
          this.app.goHome(d);
          return;
        }
        if (ev.type !== 'PRESS') return;
        if (ev.button === 'Ok') {
          this.entered = true;
          this.render();
        } else if (ev.button === 'Back') {
          this.app.goHomeTo(0);
        }
        return;
      }
      const d = navDir(ev);
      if (d) {
        this.sel = (this.sel + d + 6) % 6;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push' || ev.button === 'Ok') {
        const s = this.settings;
        if (this.sel === 0) s.brightness = s.brightness >= 100 ? 20 : s.brightness + 20;
        else if (this.sel === 1) s.ledColor = (s.ledColor + 1) % this.ledColors.length;
        else if (this.sel === 2) s.wifi = !s.wifi;
        else if (this.sel === 3) s.bluetooth = !s.bluetooth;
        else if (this.sel === 4) this.app.go('IMU_CALIB', 'left');
        else if (this.sel === 5) this.app.go('GNSS_TEST', 'left');
        this.render();
      } else if (ev.button === 'Back') {
        this.entered = false;
        this.render();
      }
    }
    getChrome() {
      this.setStatus(this.entered ? 'SETTINGS · 调整中' : 'SETTINGS · OK 进入');
      return this.chrome;
    }
  }

  /* ================= MODE SELECT ================= */
  class ModeSelectScreen extends Screen {
    build() {
      this.sel = 0;
      const r = D.div('screen-inner');
      r.appendChild(title('SELECT MODE'));
      this.boxes = [];
      [
        ['NEW TRACK', '建图 + 赛车模式'],
        ['RECORDED TRACK', '已建图赛道'],
      ].forEach((it, i) => {
        const b = pxbox('mode-box');
        b.appendChild(D.div('mode-name', it[0]));
        b.appendChild(D.div('mode-sub', it[1]));
        this.boxes.push(b);
        r.appendChild(b);
      });
      r.appendChild(hint('UP/DOWN 选择 · OK 进入 · BACK 返回'));
      this.render();
      return r;
    }
    render() {
      this.boxes.forEach((b, i) => b.classList.toggle('sel', i === this.sel));
    }
    handle(ev) {
      const d = navDir(ev);
      if (d) {
        this.sel = (this.sel + d + 2) % 2;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok')
        this.app.go(this.sel === 0 ? 'NEW_TRACK_MARK_START' : 'TRACK_LIST', 'left');
      else if (ev.button === 'Back') this.app.go('HOME', 'right');
    }
    getChrome() {
      this.setStatus('MODE SELECT');
      return this.chrome;
    }
  }

  /* ================= NEW TRACK · MARK START ================= */
  class NewTrackMarkStartScreen extends Screen {
    build() {
      this.marked = false;
      this.postTimer = 0;
      this.app.sessionSections = [];
      this.sessionTrack = new RC.Track(RC.TRACK_DEFS[0]);
      this.app.sessionTrack = this.sessionTrack;

      const r = D.div('screen-inner race-layout');
      const left = D.div('map-col');
      this.map = new RC.TrackMap(D.canvas(200, 200));
      this.map.fitPoints([], 0.3); /* unknown track: start zoned-in, no shape */
      left.appendChild(this.map.cv);
      r.appendChild(left);

      const right = D.div('side-col');
      this.box = pxbox('mark-box blink');
      this.boxText = D.div('mark-text', 'MARK\nSTART LINE');
      this.box.appendChild(this.boxText);
      right.appendChild(this.box);
      right.appendChild(hint('PUSH 标记起始点'));
      r.appendChild(right);
      return r;
    }
    update(dt) {
      super.update(dt);
      this.map.begin();
      if (!this.marked) {
        this.map.jitterDot(this._t * 3, C.COLORS.cyan);
      } else {
        /* only the marked start point is known here; no track shape yet */
        const sp = this.sessionTrack.pointAt(0);
        const st = this.sessionTrack.tangentAt(0);
        this.map.startMarkAt(sp, st, C.COLORS.yellow);
        this.map.carAt(sp, st, C.COLORS.white);
        this.postTimer -= dt;
        if (this.postTimer <= 0) {
          this.app.engine = new RC.RaceEngine(this.sessionTrack, {
            speedMul: this.app.simSpeed,
            sensor: this.app.imu,
          });
          this.app.go('RACING', 'left');
        }
      }
    }
    handle(ev) {
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push' && !this.marked) {
        this.marked = true;
        this.postTimer = 1.0;
        this.box.classList.remove('blink');
        this.box.classList.add('done');
        this.boxText.textContent = 'START LINE\nMARKED';
        this.app.toast('起始线已标记');
      } else if (ev.button === 'Back') {
        this.app.go('MODE_SELECT', 'right');
      }
    }
    getChrome() {
      this.chrome.leds = null;
      this.setStatus(this.marked ? 'START MARKED · 即将开始' : '标记起始线');
      return this.chrome;
    }
  }

  /* ================= RACING (switchable instrument panels) ================= */
  const RACE_PANELS = ['TRACK', 'TIMER', 'DASH', 'SECTOR'];

  class RacingScreen extends Screen {
    constructor(app, opts) {
      super(app, opts);
      this.mode = (opts && opts.mode) || 'new';
      this.track = app.engine.track;
      this.panel = 0;
      this._lapCount = -1;
    }
    build() {
      this.root = D.div('screen-inner race-screen');
      this.body = D.div('race-body');
      this.root.appendChild(this.body);
      this.dots = D.div('panel-dots');
      this.root.appendChild(this.dots);
      this.buildPanel();
      return this.root;
    }
    buildPanel() {
      this.body.innerHTML = '';
      this.map = null;
      this._lapCount = -1;
      const build = [this.buildTrack, this.buildTimer, this.buildDash, this.buildSector][this.panel];
      build.call(this);
      this.renderDots();
    }
    renderDots() {
      this.dots.innerHTML = '';
      RACE_PANELS.forEach((n, i) =>
        this.dots.appendChild(D.div('pdot' + (i === this.panel ? ' on' : '')))
      );
    }

    /* ---- panel 0: track map + lap list ---- */
    buildTrack() {
      const wrap = D.div('race-track');
      const left = D.div('map-col small');
      this.map = new RC.TrackMap(D.canvas(150, 200));
      this.map.setTrack(this.track);
      left.appendChild(this.map.cv);
      wrap.appendChild(left);

      const right = D.div('race-side');
      const live = D.div('race-live');
      live.appendChild(D.div('race-kicker', 'LIVE'));
      this.rTime = D.div('race-time-big', '00:00.000');
      this.rDelta = D.div('race-delta', '+0.000');
      live.appendChild(this.rTime);
      live.appendChild(this.rDelta);
      right.appendChild(live);
      this.lapsEl = D.div('laps');
      right.appendChild(this.lapsEl);
      this.footer = D.div('list-footer');
      right.appendChild(this.footer);
      wrap.appendChild(right);
      this.body.appendChild(wrap);
    }

    /* ---- panel 1: big timer ---- */
    buildTimer() {
      const wrap = D.div('race-timer');
      const top = D.div('timer-top');
      this.tLap = D.div('timer-lap', 'LAP 1');
      this.tBest = D.div('timer-best', 'BEST --:--.---');
      top.appendChild(this.tLap);
      top.appendChild(this.tBest);
      wrap.appendChild(top);
      this.tTime = D.div('huge-time', '00:00.000');
      this.tDelta = D.div('huge-delta', '+0.000');
      wrap.appendChild(this.tTime);
      wrap.appendChild(this.tDelta);
      this.tLast = D.div('timer-last', 'LAST --:--.---   ·   UP/DOWN 切换面板');
      wrap.appendChild(this.tLast);
      this.body.appendChild(wrap);
    }

    /* ---- panel 2: telemetry dash ---- */
    buildDash() {
      const wrap = D.div('race-dash');
      const left = D.div('dash-left');
      this.dSpeed = D.div('dash-speed', '0');
      left.appendChild(this.dSpeed);
      left.appendChild(D.div('dash-unit', 'km/h'));
      const bar = D.div('dash-bar');
      this.dFill = D.div('dash-bar-fill');
      bar.appendChild(this.dFill);
      left.appendChild(bar);
      this.dDist = D.div('dash-dist', '0 m');
      left.appendChild(this.dDist);
      wrap.appendChild(left);

      const mid = D.div('dash-mid');
      this.gcv = D.canvas(104, 104);
      this.gctx = this.gcv.getContext('2d');
      mid.appendChild(this.gcv);
      this.gval = D.div('dash-gval', '0.00 g');
      mid.appendChild(this.gval);
      wrap.appendChild(mid);

      const right = D.div('dash-right');
      this.dGrid = {};
      [
        ['SATS', 'dSats'],
        ['HDOP', 'dHdop'],
        ['FIX', 'dFix'],
        ['KF-σ', 'dKf'],
        ['HDG', 'dHdg'],
        ['ALT', 'dAlt'],
      ].forEach((it) => {
        const row = D.div('dash-row');
        row.appendChild(D.div('dash-k', it[0]));
        const v = D.div('dash-v', '-');
        row.appendChild(v);
        this.dGrid[it[1]] = v;
        right.appendChild(row);
      });
      wrap.appendChild(right);
      this.body.appendChild(wrap);
    }

    /* ---- panel 3: sector splits ---- */
    buildSector() {
      const wrap = D.div('race-split');
      wrap.appendChild(title('SECTOR SPLITS'));
      this.splitList = D.div('split-list');
      wrap.appendChild(this.splitList);
      this.splitInfo = hint('OK 结束比赛 · UP/DOWN 切换面板');
      wrap.appendChild(this.splitInfo);
      this.body.appendChild(wrap);
    }

    sections() {
      return (this.mode === 'recorded' ? this.track.sections : this.app.sessionSections) || [];
    }

    update(dt) {
      super.update(dt);
      const e = this.app.engine;
      e.speedMul = this.app.simSpeed;
      e.update(dt);
      this.chrome.leds = e.leds;
      if (this.panel === 0) {
        this.drawMap(e, dt);
        this.updateTrackList(e);
      } else if (this.panel === 1) {
        this.updateTimer(e);
      } else if (this.panel === 2) {
        this.updateDash(e);
      } else {
        this.updateSector(e);
      }
      let st =
        RACE_PANELS[this.panel] +
        ' ' + (this.panel + 1) + '/' + RACE_PANELS.length +
        '   LAP ' + e.lap +
        '   ' + D.fmtTime(e.elapsed);
      if (e.best) st += '   BEST ' + D.fmtTime(e.best.time);
      this.setStatus(st);
    }

    drawMap(e, dt) {
      const rec = this.mode === 'recorded';
      this.map.begin();
      if (rec) {
        /* known track: whole loop visible from the start */
        this.map.centerline();
      } else {
        /* New Track: camera follows the trace as it is being recorded */
        this.map.fitEngine(e, 0.28);
        this.map.smooth(dt || 0.016, 4);
      }
      /* raw GPS fixes (noisy) faint, then the fused (Kalman) trajectory */
      for (const lap of e.laps) if (lap.gpsTrace) this.map.trace(lap.gpsTrace, '#3a2e33', 2);
      this.map.trace(e.gpsLapTrace, '#4a3540', 2);
      this.map.trail(e);
      if (rec) {
        this.map.startLine(0, C.COLORS.white);
        for (const f of this.sections()) this.map.tick(f, C.COLORS.orange, 4);
        this.map.car(e.progress, C.COLORS.green);
      } else {
        const sp = e.track.pointAt(0);
        const st = e.track.tangentAt(0);
        this.map.startMarkAt(sp, st, C.COLORS.white);
        this.map.carAt(e.track.pointAt(e.progress), e.track.tangentAt(e.progress), C.COLORS.green);
      }
    }

    updateTrackList(e) {
      this.rTime.textContent = D.fmtTime(e.elapsed);
      this.rDelta.textContent = D.fmtDelta(e.diff);
      this.rDelta.className =
        'race-delta ' + (e.diff > 0.02 ? 'slow' : e.diff < -0.02 ? 'fast' : '');
      if (e.laps.length !== this._lapCount) {
        this._lapCount = e.laps.length;
        this.lapsEl.innerHTML = '';
        const laps = e.laps.slice().reverse().slice(0, 3);
        for (const lap of laps) {
          const best = e.best && lap.index === e.best.index;
          const row = D.div('lap-row' + (best ? ' best' : ''));
          row.appendChild(D.div('lap-name', 'L' + lap.index));
          row.appendChild(D.div('lap-time', D.fmtTime(lap.time)));
          this.lapsEl.appendChild(row);
        }
        if (!laps.length) this.lapsEl.appendChild(D.div('lap-empty', '--'));
        this.footer.innerHTML = '';
        if (this.mode === 'recorded' && this.track.best != null) {
          const row = D.div('lap-row gold');
          row.appendChild(D.div('lap-name', 'H-BEST'));
          row.appendChild(D.div('lap-time', D.fmtTime(this.track.best)));
          this.footer.appendChild(row);
        }
      }
    }

    updateTimer(e) {
      this.tLap.textContent = 'LAP ' + e.lap;
      this.tBest.textContent = 'BEST ' + (e.best ? D.fmtTime(e.best.time) : '--:--.---');
      this.tTime.textContent = D.fmtTime(e.elapsed);
      this.tDelta.textContent = D.fmtDelta(e.diff);
      this.tDelta.className =
        'huge-delta ' + (e.diff > 0.02 ? 'slow' : e.diff < -0.02 ? 'fast' : '');
      const last = e.laps.length ? e.laps[e.laps.length - 1] : null;
      this.tLast.textContent =
        'LAST ' + (last ? D.fmtTime(last.time) : '--:--.---') + '   ·   UP/DOWN 切换面板';
    }

    updateDash(e) {
      const sp = e.speedKmh();
      this.dSpeed.textContent = Math.round(sp);
      this.dFill.style.width = Math.min(100, (sp / e.track.maxSpeed) * 100) + '%';
      this.dDist.textContent = Math.round(e.distM()) + ' m';
      /* G meter */
      RC.drawGmeter(this.gctx, this.gcv.width, this.gcv.height, e.gLat, e.gLong, e.gTrail, 2);
      this.gval.textContent =
        'G ' + Math.hypot(e.gLat, e.gLong).toFixed(2) +
        '  (' + e.gLat.toFixed(1) + ',' + e.gLong.toFixed(1) + ')';
      /* telemetry grid */
      const g = this.app.gnss;
      this.dGrid.dSats.textContent = g ? g.used + '/' + g.visible : '-';
      this.dGrid.dHdop.textContent = g ? g.hdop.toFixed(1) : '-';
      this.dGrid.dHdg.textContent = Math.round(e.headingDeg()) + '°';
      this.dGrid.dAlt.textContent = e.altM().toFixed(1) + ' m';
      this.dGrid.dFix.textContent = g ? g.fix.replace(' FIX', '') : '-';
      this.dGrid.dFix.className = 'dash-v ' + (g && g.fix !== 'NO FIX' ? 'good' : 'bad');
      const f = e.fusion;
      this.dGrid.dKf.textContent = f ? f.sigma.toFixed(4) : '-';
      this.dGrid.dKf.className = 'dash-v ' + (e.gpsFix ? 'good' : 'bad');
    }

    updateSector(e) {
      const bounds = this.sections(); // section END boundaries, last = 1.0
      const deltas = e.sectionDeltas();
      const prog = e.progress;
      let cur = bounds.findIndex((f) => prog <= f);
      if (cur < 0) cur = bounds.length - 1;
      this.splitList.innerHTML = '';
      if (!bounds.length) {
        this.splitList.appendChild(D.div('lap-empty', '无 Section 数据'));
        this.splitInfo.textContent = 'OK 结束比赛 · UP/DOWN 切换面板';
        return;
      }
      let start = 0;
      bounds.forEach((end, i) => {
        const row = D.div('split-row' + (i === cur ? ' cur' : ''));
        row.appendChild(D.div('split-name', 'S' + (i + 1)));
        row.appendChild(
          D.div('split-pos', Math.round(start * 100) + '-' + Math.round(end * 100) + '%')
        );
        const d = deltas[i] ? deltas[i].delta : null;
        row.appendChild(
          D.div(
            'split-delta ' + (d == null ? '' : d > 0.02 ? 'slow' : d < -0.02 ? 'fast' : ''),
            d == null ? '--.---' : D.fmtDelta(d)
          )
        );
        this.splitList.appendChild(row);
        start = end;
      });
      this.splitInfo.textContent = bounds.length + ' 段 · OK 结束比赛';
    }

    handle(ev) {
      if (ev.type === 'PRESS' && (ev.button === 'Up' || ev.button === 'Down')) {
        const d = ev.button === 'Up' ? -1 : 1;
        this.panel = (this.panel + d + RACE_PANELS.length) % RACE_PANELS.length;
        this.buildPanel();
        this.app.toast('面板 ' + RACE_PANELS[this.panel]);
        return;
      }
      if (ev.button === 'Back' && ev.type === 'LONG') {
        this.app.engine.paused = true;
        if (this.mode === 'recorded') {
          this.app.go('HOME', 'right');
        } else {
          this.app.pushOverlay('CONFIRM', {
            text: '用此次记录\n新建赛道？',
            yesLabel: 'YES',
            noLabel: 'NO',
            onYes: () => {
              this.app.sessionLaps = this.app.engine.laps;
              this.app.go('LAP_SELECT', 'left');
            },
            onNo: () => this.app.go('HOME', 'right'),
          });
        }
      }
    }
  }

  class RacingRecordedScreen extends RacingScreen {
    constructor(app, opts) {
      super(app, Object.assign({ mode: 'recorded' }, opts || {}));
    }
    handle(ev) {
      if (ev.type === 'PRESS' && ev.button === 'Back') {
        this.app.engine.paused = true;
        this.app.go('TRACK_LIST', 'right');
        return;
      }
      super.handle(ev);
    }
  }

  /* ================= POST RACE · LAP SELECT ================= */
  class LapSelectScreen extends Screen {
    build() {
      this.laps = this.app.engine.laps.slice();
      this.checked = this.laps.map(() => true);
      this.sel = 0;
      this.K = 5;

      const r = D.div('screen-inner race-layout');
      const left = D.div('map-col');
      this.map = new RC.TrackMap(D.canvas(200, 200));
      this.map.setTrack(this.app.engine.track);
      left.appendChild(this.map.cv);
      r.appendChild(left);

      const right = D.div('side-col list-col');
      right.appendChild(title('SELECT LAPS'));
      this.list = D.div('list');
      right.appendChild(this.list);
      this.info = D.div('lap-empty', '');
      right.appendChild(this.info);
      right.appendChild(hint('PUSH 勾选 · OK 确认'));
      r.appendChild(right);
      this.render();
      return r;
    }
    render() {
      this.list.innerHTML = '';
      const w = windowed(this.laps.length, this.sel, this.K);
      for (let i = w.start; i < w.end; i++) {
        const lap = this.laps[i];
        const row = D.div('lap-row pick' + (i === this.sel ? ' cursor' : ''));
        const cb = D.div('checkbox' + (this.checked[i] ? ' on' : ''));
        row.appendChild(cb);
        row.appendChild(D.div('lap-name', 'L' + lap.index));
        row.appendChild(D.div('lap-time', D.fmtTime(lap.time)));
        this.list.appendChild(row);
      }
      const n = this.checked.filter(Boolean).length;
      this.info.textContent = '已选 ' + n + ' / 至少 2 圈';
      if (!this.laps.length) this.info.textContent = '无可用圈';
    }
    drawMap() {
      this.map.begin();
      this.map.centerline();
      this.laps.forEach((lap, i) => {
        this.map.trace(lap.trace, this.checked[i] ? '#3a5a52' : '#23232b', 2);
      });
    }
    update(dt) {
      super.update(dt);
      this.drawMap();
      this.setStatus('选择圈 · 已选 ' + this.checked.filter(Boolean).length);
    }
    handle(ev) {
      const d = navDir(ev);
      if (d && this.laps.length) {
        this.sel = (this.sel + d + this.laps.length) % this.laps.length;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push' && this.laps.length) {
        this.checked[this.sel] = !this.checked[this.sel];
        this.render();
      } else if (ev.button === 'Ok') {
        const selected = this.laps.filter((l, i) => this.checked[i]);
        if (selected.length < 2) {
          this.app.toast('至少选择 2 圈');
          return;
        }
        const avg = this.averageTraces(selected);
        this.app.sessionTrack = RC.Track.fromPoints(avg, 'NEW', this.app.sessionVehicle || 0);
        this.app.pushOverlay('CONFIRM', {
          text: '是否标记\nSection 计时段？',
          yesLabel: 'YES',
          noLabel: 'NO',
          onYes: () => this.app.go('SECTION_MARKING', 'left'),
          onNo: () => {
            this.app.sessionSections = [];
            this.app.sessionTrack.sections = [];
            this.app.go('VEHICLE_SELECT', 'left');
          },
        });
      } else if (ev.button === 'Back') {
        this.app.go('HOME', 'right');
      }
    }
    averageTraces(laps) {
      const M = 120;
      const acc = [];
      for (let i = 0; i < M; i++) acc.push({ x: 0, y: 0 });
      let used = 0;
      for (const lap of laps) {
        const tr = lap.trace;
        if (!tr || tr.length < 4) continue;
        used++;
        for (let i = 0; i < M; i++) {
          const f = (i / M) * tr.length;
          const i0 = Math.floor(f) % tr.length;
          const i1 = (i0 + 1) % tr.length;
          const t = f - Math.floor(f);
          acc[i].x += tr[i0].x + (tr[i1].x - tr[i0].x) * t;
          acc[i].y += tr[i0].y + (tr[i1].y - tr[i0].y) * t;
        }
      }
      if (!used) return laps[0].trace.slice();
      return acc.map((p) => ({ x: p.x / used, y: p.y / used }));
    }
  }

  /* ================= SECTION MARKING ================= */
  class SectionMarkingScreen extends Screen {
    build() {
      this.track = this.app.sessionTrack;
      this.sections = []; // section END boundaries; final entry is always 1.0 when finished
      this.prevBoundary = 0;
      this.markPos = 0.03;
      this.candidate = null;
      this.done = false;

      const r = D.div('screen-inner race-layout');
      const left = D.div('map-col');
      this.map = new RC.TrackMap(D.canvas(200, 200));
      this.map.setTrack(this.track);
      left.appendChild(this.map.cv);
      r.appendChild(left);

      const right = D.div('side-col');
      right.appendChild(title('SECTION'));
      this.info = D.div('section-info', '');
      right.appendChild(this.info);
      this.foot = hint('UP/DOWN 推进 · PUSH 标记');
      right.appendChild(this.foot);
      this.foot2 = hint('OK 直接结束余下为最后段');
      right.appendChild(this.foot2);
      r.appendChild(right);
      return r;
    }

    addSection(f) {
      f = D.clamp(f, 0, 1);
      if (f > this.prevBoundary + 0.005) {
        this.sections.push(f);
        this.prevBoundary = f;
        return true;
      }
      return false;
    }

    update(dt) {
      super.update(dt);
      this.map.begin();
      this.map.centerline();
      this.map.startLine(0, C.COLORS.white);
      for (const f of this.sections) this.map.tick(f, C.COLORS.orange, 6);
      if (this.candidate != null) this.map.tick(this.candidate, C.COLORS.yellow, 7);
      const blink = Math.floor(this._t * 4) % 2 === 0;
      this.map.marker(this.markPos, C.COLORS.orange, 5, blink);
      const pct = Math.round(this.markPos * 100);
      this.info.textContent =
        '已标记 ' + this.sections.length + ' 段\n' +
        '当前位置 ' + pct + '%\n' +
        '余下 ' + Math.round((1 - this.prevBoundary) * 100) + '%' +
        (this.candidate != null ? '\n候选 ' + Math.round(this.candidate * 100) + '% 待确认' : '');
      this.setStatus(
        'SECTION ' + (this.sections.length + 1) + ' · ' + pct + '%' +
        (this.candidate != null ? ' · 候选' : ' · OK=结束')
      );
    }

    handle(ev) {
      const d = navDir(ev);
      if (d) {
        const step = ev.type === 'REPEAT' || ev.type === 'LONG' ? 0.05 : 0.02;
        this.markPos = D.clamp(this.markPos + d * step, 0.01, 0.99);
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push') {
        /* toggle candidate so a stray mark can always be cancelled */
        if (this.candidate != null) {
          this.candidate = null;
          this.app.toast('已取消候选');
        } else {
          this.candidate = this.markPos;
          this.app.toast('候选点 ' + Math.round(this.markPos * 100) + '%');
        }
      } else if (ev.button === 'Ok') {
        if (this.candidate != null) {
          if (this.addSection(this.candidate)) {
            const n = this.sections.length;
            this.candidate = null;
            if (this.sections[this.sections.length - 1] >= 0.985) this.finish();
            else this.app.toast('已标记 S' + n);
          } else {
            /* invalid candidate -> drop it and treat as a finish request */
            this.candidate = null;
            this.app.toast('候选过近，已取消');
            this.promptFinish();
          }
          return;
        }
        this.promptFinish();
      } else if (ev.button === 'Back') {
        if (this.candidate != null) {
          this.candidate = null;
          this.app.toast('已取消候选');
          return;
        }
        this.app.go('LAP_SELECT', 'right');
      }
    }

    promptFinish() {
      const extra =
        this.markPos > this.prevBoundary + 0.02 && this.markPos < 0.97;
      this.app.pushOverlay('CONFIRM', {
        text: extra
          ? '当前位置到终点\n作为最后一个 Section\n（' + Math.round(this.markPos * 100) + '% → 100%），确认？'
          : '余下到终点\n作为最后一个 Section，确认？',
        yesLabel: 'OK',
        noLabel: 'BACK',
        onYes: () => {
          if (extra) this.addSection(this.markPos);
          this.finish();
        },
        onNo: () => {},
      });
    }

    finish() {
      if (!this.done) {
        this.done = true;
        this.addSection(1);
        this.app.sessionSections = this.sections.slice();
        if (this.app.sessionTrack) this.app.sessionTrack.sections = this.sections.slice();
      }
      this.app.go('VEHICLE_SELECT', 'left');
    }

    getChrome() {
      return this.chrome;
    }
  }

  /* ================= VEHICLE SELECT ================= */
  class VehicleSelectScreen extends Screen {
    build() {
      this.sel = this.app.sessionVehicle || 0;
      const r = D.div('screen-inner');
      r.appendChild(title('SELECT VEHICLE'));
      const vp = D.div('veh-viewport');
      this.strip = D.div('veh-strip');
      this.cards = [];
      C.VEHICLES.forEach((veh) => {
        const card = D.div('veh-card');
        const cv = D.canvas(72, 56);
        RC.drawVehicleIcon(cv, veh);
        card.appendChild(cv);
        card.appendChild(D.div('veh-name', veh.name));
        this.cards.push(card);
        this.strip.appendChild(card);
      });
      vp.appendChild(this.strip);
      r.appendChild(vp);
      r.appendChild(hint('UP/DOWN 选择 · OK 确认'));
      this.render();
      return r;
    }
    render() {
      const cardW = 120;
      const tx = 120 - this.sel * cardW;
      this.strip.style.transform = 'translateX(' + tx + 'px)';
      this.cards.forEach((c, i) => c.classList.toggle('sel', i === this.sel));
    }
    handle(ev) {
      const d = navDir(ev);
      if (d) {
        this.sel = (this.sel + d + C.VEHICLES.length) % C.VEHICLES.length;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok') {
        this.app.sessionVehicle = this.sel;
        this.app.go('TRACK_NAMING', 'left');
      } else if (ev.button === 'Back') {
        this.app.go('LAP_SELECT', 'right');
      }
    }
    getChrome() {
      this.setStatus('VEHICLE · ' + C.VEHICLES[this.sel].name);
      return this.chrome;
    }
  }

  /* ================= TRACK NAMING ================= */
  class TrackNamingScreen extends Screen {
    build() {
      const used = this.app.tracks.map((t) => t.name[0]);
      this.avail = C.LETTERS.split('').filter((ch) => used.indexOf(ch) < 0);
      if (!this.avail.length) this.avail = C.LETTERS.split('');
      this.sel = 0;
      const r = D.div('screen-inner');
      r.appendChild(title('NAME TRACK'));
      this.big = D.div('name-big', this.avail[0]);
      r.appendChild(this.big);
      this.row = D.div('letter-row');
      r.appendChild(this.row);
      r.appendChild(hint('UP/DOWN 换字母 · OK 保存'));
      this.render();
      return r;
    }
    render() {
      this.big.textContent = this.avail[this.sel];
      this.row.innerHTML = '';
      this.avail.forEach((ch, i) => {
        this.row.appendChild(D.div('letter' + (i === this.sel ? ' sel' : ''), ch));
      });
    }
    handle(ev) {
      const d = navDir(ev);
      if (d) {
        this.sel = (this.sel + d + this.avail.length) % this.avail.length;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok') {
        const name = this.avail[this.sel];
        const track = this.app.sessionTrack;
        track.name = name;
        track.vehicle = this.app.sessionVehicle || 0;
        track.sections = (this.app.sessionSections || []).slice();
        track.best = null;
        this.app.tracks.push(track);
        this.app.toast('赛道 ' + name + ' 已保存');
        this.app.sessionTrack = null;
        this.app.sessionSections = [];
        this.app.goHomeTo(0);
      } else if (ev.button === 'Back') {
        this.app.go('VEHICLE_SELECT', 'right');
      }
    }
    getChrome() {
      this.setStatus('命名赛道');
      return this.chrome;
    }
  }

  /* ================= TRACK LIST ================= */
  class TrackListScreen extends Screen {
    build() {
      this.cursor = 0;
      this.selected = null;
      this.K = 3;
      const r = D.div('screen-inner');
      r.appendChild(title('TRACKS'));
      this.list = D.div('list');
      r.appendChild(this.list);
      r.appendChild(hint('PUSH 选中 · OK 进入'));
      this.render();
      return r;
    }
    render() {
      this.list.innerHTML = '';
      if (!this.app.tracks.length) {
        this.list.appendChild(hint('（暂无赛道）'));
        return;
      }
      const w = windowed(this.app.tracks.length, this.cursor, this.K);
      for (let i = w.start; i < w.end; i++) {
        const t = this.app.tracks[i];
        const b = pxbox('track-row' +
          (i === this.cursor ? ' cursor' : '') +
          (i === this.selected ? ' sel' : ''));
        const cv = D.canvas(40, 30);
        RC.drawThumb(cv, t, i === this.selected ? C.COLORS.cyan : C.COLORS.dim);
        b.appendChild(cv);
        const info = D.div('track-info');
        info.appendChild(D.div('row-name', t.name));
        info.appendChild(D.div('row-sub', C.VEHICLES[t.vehicle].name));
        b.appendChild(info);
        this.list.appendChild(b);
      }
    }
    handle(ev) {
      const d = navDir(ev);
      if (d && this.app.tracks.length) {
        this.cursor = (this.cursor + d + this.app.tracks.length) % this.app.tracks.length;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push' && this.app.tracks.length) {
        this.selected = this.cursor;
        this.render();
        this.app.toast('已选中 ' + this.app.tracks[this.cursor].name);
      } else if (ev.button === 'Ok') {
        const idx = this.selected != null ? this.selected : this.cursor;
        const t = this.app.tracks[idx];
        if (!t) return;
        this.app.engine = new RC.RaceEngine(t, {
          speedMul: this.app.simSpeed,
          histBest: t.best,
          sensor: this.app.imu,
        });
        this.app.go('RACING_RECORDED', 'left');
      } else if (ev.button === 'Back') {
        this.app.go('HOME', 'right');
      }
    }
    getChrome() {
      this.setStatus('TRACK LIST · ' + this.app.tracks.length + ' 条');
      return this.chrome;
    }
  }

  /* ================= GNSS TEST (NEO-M9N) ================= */
  class GnssTestScreen extends Screen {
    build() {
      const r = D.div('screen-inner gnss-screen');
      const left = D.div('gnss-sky');
      this.cv = D.canvas(180, 200);
      this.ctx = this.cv.getContext('2d');
      this.ctx.imageSmoothingEnabled = false;
      left.appendChild(this.cv);
      r.appendChild(left);

      const right = D.div('gnss-info');
      right.appendChild(D.div('gnss-title', 'NEO-M9N'));
      this.fixEl = D.div('gnss-fix', 'NO FIX');
      right.appendChild(this.fixEl);
      this.metaEl = D.div('gnss-meta', '');
      right.appendChild(this.metaEl);
      this.posEl = D.div('gnss-pos', '');
      right.appendChild(this.posEl);
      this.satList = D.div('gnss-sat-list');
      right.appendChild(this.satList);
      right.appendChild(hint('PUSH 重新搜索 · BACK 返回'));
      r.appendChild(right);
      return r;
    }

    update(dt) {
      super.update(dt);
      const g = this.app.gnss;
      this.drawSky(g);
      this.drawInfo(g);
      this.setStatus(
        'GNSS · ' + g.fix + ' · ' + g.used + '/' + g.visible + ' SAT · HDOP ' + g.hdop.toFixed(1),
        g.fix === 'NO FIX' ? C.COLORS.red : C.COLORS.green
      );
    }

    drawSky(g) {
      const c = this.ctx;
      const W = this.cv.width;
      const H = this.cv.height;
      c.fillStyle = C.COLORS.stage;
      c.fillRect(0, 0, W, H);
      const cx = W / 2;
      const cy = H / 2;
      const R = Math.min(W, H) / 2 - 16;
      c.strokeStyle = '#1e1e29';
      c.lineWidth = 1;
      [1, 2 / 3, 1 / 3].forEach((k) => {
        c.beginPath();
        c.arc(cx, cy, R * k, 0, Math.PI * 2);
        c.stroke();
      });
      c.beginPath();
      c.moveTo(cx - R, cy);
      c.lineTo(cx + R, cy);
      c.moveTo(cx, cy - R);
      c.lineTo(cx, cy + R);
      c.stroke();
      /* N marker */
      c.fillStyle = C.COLORS.dim;
      c.fillRect(cx - 1, cy - R - 3, 2, 6);
      /* horizon center */
      c.fillStyle = '#2b2b33';
      c.fillRect(cx - 1, cy - 1, 2, 2);
      for (const s of g.sats) {
        if (s.cn0 <= 0.5) continue;
        const rr = ((90 - s.el) / 90) * R;
        const a = ((s.az - 90) * Math.PI) / 180;
        const x = cx + Math.cos(a) * rr;
        const y = cy + Math.sin(a) * rr;
        const col = s.cn0 >= 40 ? C.COLORS.green : s.cn0 >= 30 ? C.COLORS.yellow : C.COLORS.red;
        const size = s.used ? 4 : 3;
        c.fillStyle = col;
        c.fillRect(Math.round(x / 2) * 2 - 1, Math.round(y / 2) * 2 - 1, size, size);
      }
    }

    drawInfo(g) {
      this.fixEl.textContent = g.fix;
      this.fixEl.className = 'gnss-fix ' + (g.fix === 'NO FIX' ? 'bad' : 'good');
      this.metaEl.textContent =
        'SATS ' + g.used + '/' + g.visible + '   HDOP ' + g.hdop.toFixed(2);
      this.posEl.textContent =
        'LAT ' + g.fmtLat() + '\nLON ' + g.fmtLon() + '\nALT ' + g.alt.toFixed(1) +
        'm  V ' + g.speed.toFixed(1) + 'm/s';
      /* top satellites by C/N0 */
      const list = g.sats
        .filter((s) => s.cn0 > 0.5)
        .sort((a, b) => b.cn0 - a.cn0)
        .slice(0, 6);
      this.satList.innerHTML = '';
      for (const s of list) {
        const row = D.div('gsat' + (s.used ? ' used' : ''));
        row.appendChild(D.div('gsat-prn', String(s.prn).padStart(2, '0')));
        const bar = D.div('gsat-bar');
        const fill = D.div('gsat-bar-fill');
        fill.style.width = Math.min(100, (s.cn0 / 50) * 100) + '%';
        fill.style.background =
          s.cn0 >= 40 ? C.COLORS.green : s.cn0 >= 30 ? C.COLORS.yellow : C.COLORS.red;
        bar.appendChild(fill);
        row.appendChild(bar);
        row.appendChild(D.div('gsat-cn0', Math.round(s.cn0)));
        this.satList.appendChild(row);
      }
      if (!list.length) this.satList.appendChild(D.div('lap-empty', '搜索卫星中…'));
    }

    handle(ev) {
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Push') {
        this.app.gnss.reset();
        this.app.toast('重新搜索 GNSS');
      } else if (ev.button === 'Back') {
        this.app.go('SETTINGS', 'right');
      }
    }
  }

  /* ================= IMU LEVEL CALIBRATION (MPU6050) ================= */
  class ImuCalibScreen extends Screen {
    build() {
      this.phase = 'idle'; // idle -> sampling -> done
      this.samples = [];
      this.progress = 0;
      this.stable = false;
      this.doneTimer = 0;
      this.mean = null;

      const r = D.div('screen-inner race-layout');
      const left = D.div('map-col');
      this.cv = D.canvas(200, 200);
      this.ctx = this.cv.getContext('2d');
      this.ctx.imageSmoothingEnabled = false;
      left.appendChild(this.cv);
      r.appendChild(left);

      const right = D.div('side-col');
      right.appendChild(title('IMU CALIB'));
      right.appendChild(D.div('imu-sub', 'MPU6050 · 6-AXIS'));
      this.stateEl = D.div('imu-state', 'PLACE FLAT');
      right.appendChild(this.stateEl);
      this.valsEl = D.div('imu-vals', '');
      right.appendChild(this.valsEl);
      const bar = D.div('imu-bar');
      this.barFill = D.div('imu-bar-fill');
      bar.appendChild(this.barFill);
      right.appendChild(bar);
      this.pctEl = D.div('imu-pct', '0%');
      right.appendChild(this.pctEl);
      right.appendChild(hint('OK 立即完成 · PUSH 重采'));
      right.appendChild(hint('BACK 跳过（用上次校准）'));
      this.hintEl = hint('将设备水平静置');
      right.appendChild(this.hintEl);
      r.appendChild(right);
      return r;
    }

    onMount() {
      this.phase = 'sampling';
      this.app.imu.clearCalibration();
    }

    _std(arr, key) {
      if (arr.length < 8) return 0;
      let m = 0;
      for (const s of arr) m += s[key];
      m /= arr.length;
      let v = 0;
      for (const s of arr) v += (s[key] - m) * (s[key] - m);
      return Math.sqrt(v / arr.length);
    }

    update(dt) {
      super.update(dt);
      const sensor = this.app.imu;
      if (this.phase === 'sampling') {
        this.samples.push(sensor.sampleStatic());
        if (this.samples.length > 100) this.samples.shift();
        const stA = Math.max(
          this._std(this.samples, 'ax'),
          this._std(this.samples, 'ay'),
          this._std(this.samples, 'az')
        );
        const stG = Math.max(
          this._std(this.samples, 'gx'),
          this._std(this.samples, 'gy'),
          this._std(this.samples, 'gz')
        );
        this.stable = stA < C.IMU.STABLE_TILT && stG < C.IMU.STABLE_GYRO;
        this.progress = D.clamp(
          this.progress + (this.stable ? dt * 1000 : -dt * 600),
          0,
          C.IMU.CALIB_MS
        );
        if (this.progress >= C.IMU.CALIB_MS) this.complete();
      } else if (this.phase === 'done') {
        this.doneTimer -= dt;
        if (this.doneTimer <= 0) this.app.goHomeTo(0);
      }
      this.draw();
      this.drawInfo();
      this.setStatus(
        this.phase === 'done'
          ? 'IMU 校准完成'
          : 'IMU CALIB · ' + (this.stable ? 'STABLE' : 'MOVING') + ' · ' +
            Math.round((this.progress / C.IMU.CALIB_MS) * 100) + '%',
        this.stable ? C.COLORS.green : C.COLORS.yellow
      );
    }

    draw() {
      const c = this.ctx;
      const W = this.cv.width;
      const H = this.cv.height;
      c.fillStyle = C.COLORS.stage;
      c.fillRect(0, 0, W, H);
      const cx = W / 2;
      const cy = H / 2;
      const R = Math.min(W, H) / 2 - 18;
      const scale = R / 0.15; // full radius = 0.15 g
      /* rings + cross */
      c.strokeStyle = '#23232b';
      c.lineWidth = 1;
      [0.05, 0.1, 0.15].forEach((g) => {
        c.beginPath();
        c.arc(cx, cy, g * scale, 0, Math.PI * 2);
        c.stroke();
      });
      c.beginPath();
      c.moveTo(cx - R, cy);
      c.lineTo(cx + R, cy);
      c.moveTo(cx, cy - R);
      c.lineTo(cx, cy + R);
      c.stroke();
      /* horizon cross target */
      c.fillStyle = '#2b2b33';
      c.fillRect(cx - 6, cy - 1, 12, 2);
      c.fillRect(cx - 1, cy - 6, 2, 12);
      /* bubble = measured horizontal accel */
      const s = this.samples.length ? this.samples[this.samples.length - 1] : null;
      if (s) {
        const bx = cx + s.ax * scale;
        const by = cy + s.ay * scale;
        const off = Math.hypot(s.ax, s.ay);
        c.fillStyle = off < 0.03 ? C.COLORS.green : off < 0.08 ? C.COLORS.yellow : C.COLORS.red;
        c.fillRect(Math.round(bx) - 3, Math.round(by) - 3, 6, 6);
        c.fillStyle = '#0b0b0d';
        c.fillRect(Math.round(bx) - 1, Math.round(by) - 1, 2, 2);
      }
      /* progress arc */
      c.strokeStyle = this.phase === 'done' ? C.COLORS.green : C.COLORS.cyan;
      c.lineWidth = 3;
      c.beginPath();
      c.arc(
        cx,
        cy,
        R + 6,
        -Math.PI / 2,
        -Math.PI / 2 + (this.progress / C.IMU.CALIB_MS) * Math.PI * 2
      );
      c.stroke();
    }

    drawInfo() {
      const s = this.samples.length ? this.samples[this.samples.length - 1] : null;
      if (s) {
        this.valsEl.textContent =
          'A ' + s.ax.toFixed(3) + ' ' + s.ay.toFixed(3) + ' ' + s.az.toFixed(3) + '\n' +
          'G ' + s.gx.toFixed(1) + ' ' + s.gy.toFixed(1) + ' ' + s.gz.toFixed(1);
      }
      this.barFill.style.width =
        Math.round((this.progress / C.IMU.CALIB_MS) * 100) + '%';
      this.pctEl.textContent = Math.round((this.progress / C.IMU.CALIB_MS) * 100) + '%';
      if (this.phase === 'done') {
        this.stateEl.textContent = 'DONE';
        this.stateEl.className = 'imu-state good';
        this.hintEl.textContent = '已保存零偏，即将返回';
      } else {
        this.stateEl.textContent = this.stable ? 'STABLE' : 'MOVING';
        this.stateEl.className = 'imu-state ' + (this.stable ? 'good' : 'bad');
      }
    }

    complete() {
      if (this.phase === 'done') return;
      /* average the stable window */
      const n = this.samples.length;
      if (n) {
        const m = { ax: 0, ay: 0, az: 0, gx: 0, gy: 0, gz: 0 };
        for (const s of this.samples) {
          for (const k in m) m[k] += s[k];
        }
        for (const k in m) m[k] /= n;
        this.mean = m;
        this.app.imu.applyCalibration(m);
      }
      this.phase = 'done';
      this.doneTimer = 0.8;
      this.progress = C.IMU.CALIB_MS;
      this.app.toast('IMU 校准完成');
    }

    handle(ev) {
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok') {
        this.complete();
      } else if (ev.button === 'Push') {
        this.samples = [];
        this.progress = 0;
        this.phase = 'sampling';
        this.app.imu.clearCalibration();
        this.app.toast('重新采样');
      } else if (ev.button === 'Back') {
        this.app.goHomeTo(0);
      }
    }
  }

  /* ================= CONFIRM MODAL (overlay) ================= */
  class ConfirmModal extends Screen {
    constructor(app, opts) {
      super(app, opts);
      this.modal = true;
    }
    build() {
      this.sel = this.opts.defaultIndex || 0;
      const r = D.div('modal-card');
      r.appendChild(D.div('modal-title', this.opts.text || ''));
      const wrap = D.div('modal-btns');
      this.boxes = [];
      [this.opts.yesLabel || 'YES', this.opts.noLabel || 'NO'].forEach((lab) => {
        const b = pxbox('modal-btn');
        b.appendChild(D.div('modal-btn-t', lab));
        this.boxes.push(b);
        wrap.appendChild(b);
      });
      r.appendChild(wrap);
      this.render();
      return r;
    }
    render() {
      this.boxes.forEach((b, i) => b.classList.toggle('sel', i === this.sel));
    }
    handle(ev) {
      const d = navDir(ev);
      if (d) {
        this.sel = (this.sel + d + 2) % 2;
        this.render();
        return;
      }
      if (ev.type !== 'PRESS') return;
      if (ev.button === 'Ok') this.choose(this.sel === 0);
      else if (ev.button === 'Back') this.choose(false);
    }
    choose(yes) {
      const o = this.opts;
      this.app.popOverlay('down');
      if (yes) {
        if (o.onYes) o.onYes();
      } else if (o.onNo) o.onNo();
    }
    getChrome() {
      return this.chrome;
    }
  }

  RC.SCREENS = {
    HOME: HomeScreen,
    TRACK_EDIT: TrackEditScreen,
    SETTINGS: SettingsScreen,
    MODE_SELECT: ModeSelectScreen,
    NEW_TRACK_MARK_START: NewTrackMarkStartScreen,
    RACING: RacingScreen,
    RACING_RECORDED: RacingRecordedScreen,
    LAP_SELECT: LapSelectScreen,
    SECTION_MARKING: SectionMarkingScreen,
    VEHICLE_SELECT: VehicleSelectScreen,
    TRACK_NAMING: TrackNamingScreen,
    TRACK_LIST: TrackListScreen,
    GNSS_TEST: GnssTestScreen,
    IMU_CALIB: ImuCalibScreen,
    CONFIRM: ConfirmModal,
  };
})();
