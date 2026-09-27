/* Track geometry + race simulation engine.
 *
 * Track holds a closed loop resampled to N equidistant points (so point index
 * maps directly to lap fraction). RaceEngine drives a virtual car around it and
 * produces lap times / live section-time deltas for the LED diff bar.
 */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;

  /* ---------- Catmull-Rom closed spline ---------- */
  function catmull(p0, p1, p2, p3, t) {
    const t2 = t * t;
    const t3 = t2 * t;
    return {
      x:
        0.5 *
        (2 * p1.x +
          (-p0.x + p2.x) * t +
          (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
          (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3),
      y:
        0.5 *
        (2 * p1.y +
          (-p0.y + p2.y) * t +
          (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
          (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3),
    };
  }

  function catmullDense(ctrl, step) {
    const m = ctrl.length;
    const out = [];
    for (let i = 0; i < m; i++) {
      const p0 = ctrl[(i - 1 + m) % m];
      const p1 = ctrl[i];
      const p2 = ctrl[(i + 1) % m];
      const p3 = ctrl[(i + 2) % m];
      for (let j = 0; j < step; j++) out.push(catmull(p0, p1, p2, p3, j / step));
    }
    return out;
  }

  /* resample a closed polyline to N equal-arc-length points */
  function resampleClosed(dense, N) {
    const m = dense.length;
    const cum = [0];
    for (let i = 1; i <= m; i++) {
      const a = dense[i - 1];
      const b = dense[i % m];
      cum.push(cum[i - 1] + Math.hypot(b.x - a.x, b.y - a.y));
    }
    const total = cum[m] || 1e-9;
    const pts = [];
    let k = 0;
    for (let i = 0; i < N; i++) {
      const target = (total * i) / N;
      while (k < m - 1 && cum[k + 1] < target) k++;
      const segLen = cum[k + 1] - cum[k] || 1e-9;
      const t = (target - cum[k]) / segLen;
      const a = dense[k % m];
      const b = dense[(k + 1) % m];
      pts.push({ x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t });
    }
    return pts;
  }

  function normalize(pts) {
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (const p of pts) {
      if (p.x < minX) minX = p.x;
      if (p.x > maxX) maxX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.y > maxY) maxY = p.y;
    }
    const rx = maxX - minX || 1;
    const ry = maxY - minY || 1;
    const scale = Math.min(0.92 / rx, 0.92 / ry);
    const ox = (1 - rx * scale) / 2 - minX * scale;
    const oy = (1 - ry * scale) / 2 - minY * scale;
    for (const p of pts) {
      p.x = p.x * scale + ox;
      p.y = p.y * scale + oy;
    }
    return pts;
  }

  class Track {
    constructor(def) {
      this.N = def.N || 360;
      this.name = def.name || 'TRACK';
      this.vehicle = def.vehicle != null ? def.vehicle : 0;
      this.best = def.best != null ? def.best : null; // historical best (s)
      this.lapTarget = def.lapTarget || 9.5; // demo lap length in seconds

      let dense;
      if (def.points) dense = def.points;
      else {
        const ctrl = def.control.map((p) => (Array.isArray(p) ? { x: p[0], y: p[1] } : p));
        dense = catmullDense(ctrl, 24);
      }
      this.points = normalize(resampleClosed(dense, this.N));
      this.finalize();
    }

    static fromPoints(points, name, vehicle) {
      const t = Object.create(Track.prototype);
      t.N = 360;
      t.name = name;
      t.vehicle = vehicle;
      t.best = null;
      t.lapTarget = 9.5;
      t.points = normalize(resampleClosed(points, t.N));
      t.finalize();
      return t;
    }

    finalize() {
      const N = this.N;
      const pts = this.points;
      /* curvature -> speed factor */
      const curv = new Array(N).fill(0);
      for (let i = 0; i < N; i++) {
        const a = pts[(i - 1 + N) % N];
        const b = pts[i];
        const c = pts[(i + 1) % N];
        const v1x = b.x - a.x, v1y = b.y - a.y;
        const v2x = c.x - b.x, v2y = c.y - b.y;
        let d = Math.atan2(v2y, v2x) - Math.atan2(v1y, v1x);
        while (d > Math.PI) d -= 2 * Math.PI;
        while (d < -Math.PI) d += 2 * Math.PI;
        const ds = (Math.hypot(v1x, v1y) + Math.hypot(v2x, v2y)) / 2 || 1e-6;
        curv[i] = Math.abs(d) / ds;
      }
      const maxc = Math.max.apply(null, curv.concat([1e-6]));
      this.curvNorm = curv.map((k) => k / maxc);
      this.speedFactor = curv.map((k) => Math.max(0.42, 1 - 0.8 * (k / maxc)));
      const avg = this.speedFactor.reduce((a, b) => a + b, 0) / N;
      this.vTop = N / (avg * this.lapTarget);
      this.accel = this.vTop / 1.1;
      this.brake = this.vTop / 0.7;
      /* cumulative arc length (normalized units) */
      this.cum = [0];
      for (let i = 0; i < N; i++) {
        const a = pts[i];
        const b = pts[(i + 1) % N];
        this.cum.push(this.cum[i] + Math.hypot(b.x - a.x, b.y - a.y));
      }
      this.total = this.cum[N];
      /* fake physical scale for the demo dashboard */
      if (this.maxSpeed == null) this.maxSpeed = this.kart ? 62 : 78;
      if (this.lengthM == null) this.lengthM = 1180;
      return this;
    }

    pointAt(f) {
      const N = this.N;
      f = ((f % 1) + 1) % 1;
      const i = f * N;
      const i0 = Math.floor(i) % N;
      const i1 = (i0 + 1) % N;
      const t = i - Math.floor(i);
      const a = this.points[i0];
      const b = this.points[i1];
      return { x: a.x + (b.x - a.x) * t, y: a.y + (b.y - a.y) * t };
    }
    tangentAt(f) {
      const a = this.pointAt(f - 0.004);
      const b = this.pointAt(f + 0.004);
      const dx = b.x - a.x, dy = b.y - a.y;
      const len = Math.hypot(dx, dy) || 1;
      return { x: dx / len, y: dy / len };
    }
  }

  /* ================= RaceEngine ================= */
  class RaceEngine {
    constructor(track, opts) {
      opts = opts || {};
      this.track = track;
      this.N = track.N;
      this.traceEvery = opts.traceEvery || 3;
      this.speedMul = opts.speedMul || 1;
      this.histBest = opts.histBest != null ? opts.histBest : null;
      this.reset();
    }
    reset() {
      this.track = this.track;
      this.v = this.track.vTop * 0.5;
      this.s = 0.5;
      this.lap = 1;
      this.elapsed = 0;
      this.time = 0;
      this.laps = [];
      this.currentTimes = new Array(this.N).fill(null);
      this.lapTrace = [];
      this.best = null;
      this.diff = 0;
      this.leds = new Array(C.LED_COUNT).fill('off');
      this.paused = false;
      this.running = true;
    }

    update(dtSec) {
      if (this.paused || !this.running) return;
      dtSec *= this.speedMul;
      this.time += dtSec;
      this.elapsed += dtSec;
      let remaining = dtSec;
      let guard = 0;
      while (remaining > 0 && guard++ < 64) {
        const step = Math.min(remaining, 0.03);
        this.step(step);
        remaining -= step;
      }
      this.computeDiff();
    }

    step(dt) {
      const N = this.N;
      const idx = Math.floor(this.s) % N;
      const target = this.track.speedFactor[idx] * this.track.vTop;
      const rate = target > this.v ? this.track.accel : this.track.brake;
      const dv = target - this.v;
      this.v += Math.max(-rate * dt, Math.min(rate * dt, dv));
      const prevS = this.s;
      this.s += this.v * dt;
      const from = Math.floor(prevS);
      const to = Math.floor(this.s);
      if (to > from) {
        for (let j = from + 1; j <= to; j++) {
          const jj = ((j % N) + N) % N;
          this.currentTimes[jj] = this.elapsed;
          if (jj % this.traceEvery === 0) {
            const p = this.track.points[jj];
            this.lapTrace.push({ x: p.x, y: p.y });
          }
        }
      }
      if (this.s >= N) {
        this.s -= N;
        this.completeLap();
      }
    }

    fillTimes() {
      const t = this.currentTimes;
      const N = this.N;
      for (let i = 0; i < N; i++) {
        if (t[i] != null) continue;
        let p = i - 1;
        while (p >= 0 && t[p] == null) p--;
        let n = i + 1;
        while (n < N && t[n] == null) n++;
        if (p >= 0 && n < N) t[i] = t[p] + ((t[n] - t[p]) * (i - p)) / (n - p);
        else if (p >= 0) t[i] = t[p];
        else if (n < N) t[i] = t[n];
        else t[i] = 0;
      }
    }

    completeLap() {
      const lapTime = this.elapsed;
      this.fillTimes();
      const lap = {
        index: this.laps.length + 1,
        time: lapTime,
        times: this.currentTimes.slice(),
        trace: this.lapTrace.slice(),
        valid: lapTime > 1,
      };
      this.laps.push(lap);
      if (lap.valid && (!this.best || lapTime < this.best.time)) this.best = lap;
      this.elapsed = 0;
      this.lap++;
      this.currentTimes = new Array(this.N).fill(null);
      this.lapTrace = [];
    }

    computeDiff() {
      if (!this.best || !this.best.times) {
        this.diff = 0;
      } else {
        const idx = Math.floor(this.s) % this.N;
        const bt = this.best.times[idx];
        this.diff = bt != null ? this.elapsed - bt : 0;
      }
      this.leds = this.ledState(this.diff);
    }

    ledState(diff) {
      const out = new Array(C.LED_COUNT).fill('off');
      if (diff == null || Math.abs(diff) < 0.02) return out;
      const n = Math.min(C.MAX_LED, Math.max(1, Math.ceil(Math.abs(diff) / C.DIFF_PER_LED)));
      if (diff > 0) {
        for (let i = 0; i < n; i++) out[4 - i] = 'red'; // slower -> left, from center out
      } else {
        for (let i = 0; i < n; i++) out[5 + i] = 'green'; // faster -> right
      }
      return out;
    }

    get progress() {
      return this.s / this.N;
    }

    /* ---------- dashboard telemetry (simulated from the car model) ---------- */
    speedKmh() {
      return (this.v / this.track.vTop) * this.track.maxSpeed;
    }
    headingDeg() {
      const t = this.track.tangentAt(this.progress);
      return ((Math.atan2(t.x, -t.y) * 180) / Math.PI + 360) % 360;
    }
    distM() {
      return this.progress * this.track.lengthM;
    }
    altM() {
      return 84 + Math.sin(this.progress * Math.PI * 4) * 6 + Math.sin(this.time * 0.4) * 0.3;
    }
    latG() {
      const idx = Math.floor(this.s) % this.N;
      const kn = this.track.curvNorm[idx] || 0;
      const sp = this.v / this.track.vTop;
      return kn * sp * sp * 2.2;
    }
    /* section split deltas vs the best lap (fractions of the lap) */
    sectionDeltas() {
      const secs = this.track.sections || [];
      if (!this.best) return secs.map((f) => ({ f, delta: null }));
      return secs.map((f) => {
        const idx = Math.min(this.N - 1, Math.floor(f * this.N));
        const t = this.elapsed >= 0 && f <= this.progress + 0.001 ? this._timeAt(f) : null;
        const bt = this.best.times[idx];
        return { f, delta: t != null && bt != null ? t - bt : null };
      });
    }
    _timeAt(f) {
      const idx = Math.min(this.N - 1, Math.floor(f * this.N));
      const t = this.currentTimes[idx];
      return t == null ? null : t;
    }
  }

  /* ================= demo track definitions ================= */
  RC.TRACK_DEFS = [
    {
      name: 'A',
      vehicle: 0,
      best: 11.842,
      control: [
        [0.5, 0.08], [0.84, 0.19], [0.88, 0.47], [0.62, 0.53],
        [0.85, 0.72], [0.74, 0.91], [0.4, 0.9], [0.16, 0.73],
        [0.18, 0.4], [0.3, 0.15],
      ],
    },
    {
      name: 'B',
      vehicle: 3,
      best: 13.207,
      control: [
        [0.5, 0.1], [0.81, 0.26], [0.7, 0.5], [0.86, 0.7],
        [0.6, 0.9], [0.3, 0.86], [0.15, 0.6], [0.36, 0.45], [0.2, 0.24],
      ],
    },
    {
      name: 'C',
      vehicle: 1,
      best: 10.554,
      control: [
        [0.5, 0.12], [0.76, 0.16], [0.89, 0.4], [0.7, 0.6],
        [0.82, 0.85], [0.5, 0.92], [0.2, 0.8], [0.12, 0.5],
        [0.32, 0.35], [0.25, 0.2],
      ],
    },
  ];

  RC.Track = Track;
  RC.RaceEngine = RaceEngine;
})();
