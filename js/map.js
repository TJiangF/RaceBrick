/* Pixel track-map renderer + small icon painters. */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;

  const GRID = 8;

  class TrackMap {
    constructor(cv) {
      this.cv = cv;
      this.ctx = cv.getContext('2d');
      this.ctx.imageSmoothingEnabled = false;
      this.W = cv.width;
      this.H = cv.height;
      this.track = null;
      /* current view transform (world -> screen, uniform scale) */
      this.scale = 1;
      this.ox = 0;
      this.oy = 0;
      /* target transform, eased toward each frame for a smooth "camera" */
      this.tScale = 1;
      this.tOx = 0;
      this.tOy = 0;
      this.has = false;
    }

    /* known track: fit immediately to the full loop */
    setTrack(track) {
      this.track = track;
      this._fit(track.points, 0);
      this._commit();
    }

    /* unknown track (New Track): fit to whatever has been recorded so far.
     * Call every frame with the growing point cloud; the view eases in. */
    fitPoints(points, minSpan) {
      this._fit(points, minSpan || 0);
      if (!this.has) this._commit();
    }

    _fit(points, minSpan) {
      const W = this.W, H = this.H, pad = 18;
      if (!points || !points.length) {
        const ms = minSpan || 0.25;
        this.tScale = (Math.min(W, H) - 2 * pad) / ms;
        this.tOx = W / 2 - 0.5 * this.tScale;
        this.tOy = H / 2 - 0.5 * this.tScale;
        return;
      }
      let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
      for (const p of points) {
        if (p.x < minX) minX = p.x;
        if (p.x > maxX) maxX = p.x;
        if (p.y < minY) minY = p.y;
        if (p.y > maxY) maxY = p.y;
      }
      let spanX = maxX - minX;
      let spanY = maxY - minY;
      const ms = minSpan || 0;
      if (spanX < ms) {
        const c = (minX + maxX) / 2;
        minX = c - ms / 2;
        spanX = ms;
      }
      if (spanY < ms) {
        const c = (minY + maxY) / 2;
        minY = c - ms / 2;
        spanY = ms;
      }
      const s = Math.min((W - 2 * pad) / (spanX || 1), (H - 2 * pad) / (spanY || 1));
      this.tScale = s;
      this.tOx = (W - spanX * s) / 2 - minX * s;
      this.tOy = (H - spanY * s) / 2 - minY * s;
    }

    _commit() {
      this.scale = this.tScale;
      this.ox = this.tOx;
      this.oy = this.tOy;
      this.has = true;
    }

    smooth(dt, rate) {
      if (!this.has) {
        this._commit();
        return;
      }
      const k = 1 - Math.exp(-(rate || 5) * dt);
      this.scale += (this.tScale - this.scale) * k;
      this.ox += (this.tOx - this.ox) * k;
      this.oy += (this.tOy - this.oy) * k;
    }

    px(p) {
      return { x: p.x * this.scale + this.ox, y: p.y * this.scale + this.oy };
    }
    pointPx(f) {
      return this.px(this.track.pointAt(f));
    }
    tangentPx(f) {
      const a = this.pointPx(f - 0.004);
      const b = this.pointPx(f + 0.004);
      const dx = b.x - a.x, dy = b.y - a.y;
      const len = Math.hypot(dx, dy) || 1;
      return { x: dx / len, y: dy / len };
    }
    /* quantize to 2px grid for crisp pixel look */
    block(x, y, size, color) {
      const c = this.ctx;
      c.fillStyle = color;
      c.fillRect(Math.round(x / 2) * 2, Math.round(y / 2) * 2, size, size);
    }

    begin() {
      const c = this.ctx;
      c.fillStyle = C.COLORS.mapBg;
      c.fillRect(0, 0, this.W, this.H);
      c.fillStyle = C.COLORS.mapGrid;
      for (let y = GRID; y < this.H; y += GRID)
        for (let x = GRID; x < this.W; x += GRID) c.fillRect(x, y, 1, 1);
      /* subtle border */
      c.fillStyle = C.COLORS.mapBorder;
      c.fillRect(0, 0, this.W, 1);
      c.fillRect(0, this.H - 1, this.W, 1);
      c.fillRect(0, 0, 1, this.H);
      c.fillRect(this.W - 1, 0, 1, this.H);
    }

    centerline(color, step) {
      if (!this.track) return;
      const c = this.ctx;
      c.fillStyle = color || C.COLORS.centerline;
      const st = step || 2;
      for (let i = 0; i < this.track.N; i += st) {
        const p = this.px(this.track.points[i]);
        c.fillRect(Math.round(p.x / 2) * 2, Math.round(p.y / 2) * 2, 2, 2);
      }
    }

    trace(points, color, size) {
      const c = this.ctx;
      c.fillStyle = color;
      const s = size || 2;
      for (const p of points) {
        const q = this.px(p);
        c.fillRect(Math.round(q.x / 2) * 2, Math.round(q.y / 2) * 2, s, s);
      }
    }

    /* history traces dim, current lap bright */
    trail(engine) {
      for (const lap of engine.laps) this.trace(lap.trace, C.COLORS.histTrace, 2);
      this.trace(engine.lapTrace, C.COLORS.curTrace, 2);
    }

    /* fit the view to everything recorded so far (New Track mode) */
    fitEngine(engine, minSpan) {
      const pts = [];
      for (const lap of engine.laps) for (const p of lap.trace) pts.push(p);
      for (const p of engine.lapTrace) pts.push(p);
      this.fitPoints(pts, minSpan);
    }

    startLine(f, color) {
      if (!this.track) return;
      const c = this.ctx;
      const p = this.pointPx(f || 0);
      const t = this.tangentPx(f || 0);
      const nx = -t.y, ny = t.x;
      c.fillStyle = color || C.COLORS.white;
      for (let d = -9; d <= 9; d += 2) {
        this.block(p.x + nx * d, p.y + ny * d, 2, color || C.COLORS.white);
      }
    }

    tick(f, color, len) {
      if (!this.track) return;
      const p = this.pointPx(f);
      const t = this.tangentPx(f);
      const nx = -t.y, ny = t.x;
      const L = len || 5;
      for (let d = -L; d <= L; d += 2) this.block(p.x + nx * d, p.y + ny * d, 2, color);
    }

    marker(f, color, size, blinkOn) {
      if (!this.track) return;
      if (blinkOn === false) return;
      const p = this.pointPx(f);
      this.block(p.x - 1, p.y - 1, size || 4, color);
      this.block(p.x - 3, p.y - 1, 2, color);
      this.block(p.x + 1, p.y - 1, 2, color);
    }

    car(f, color) {
      if (!this.track) return;
      this.carAt(this.track.pointAt(f), this.track.tangentAt(f), color);
    }

    /* point-based variants (used when the full track is unknown) */
    carAt(pworld, tangentWorld, color) {
      const p = this.px(pworld);
      const t = tangentWorld || { x: 0, y: 0 };
      this.block(p.x + t.x * 5 - 1, p.y + t.y * 5 - 1, 2, color);
      this.block(p.x - 2, p.y - 2, 4, C.COLORS.white);
      this.block(p.x - 1, p.y - 1, 2, color);
    }

    startMarkAt(pworld, tangentWorld, color) {
      const p = this.px(pworld);
      const t = tangentWorld || { x: 1, y: 0 };
      const nx = -t.y, ny = t.x;
      for (let d = -9; d <= 9; d += 2) {
        this.block(p.x + nx * d, p.y + ny * d, 2, color || C.COLORS.white);
      }
    }

    markerAt(pworld, color, size, blinkOn) {
      if (blinkOn === false) return;
      const p = this.px(pworld);
      this.block(p.x - 1, p.y - 1, size || 4, color);
      this.block(p.x - 3, p.y - 1, 2, color);
      this.block(p.x + 1, p.y - 1, 2, color);
    }

    /* GPS jitter dot for the "before start" state */
    jitterDot(seed, color) {
      const cx = this.W * 0.5, cy = this.H * 0.52;
      const jx = Math.sin(seed * 7.3) * 3 + Math.sin(seed * 2.1) * 2;
      const jy = Math.cos(seed * 5.7) * 3 + Math.cos(seed * 1.7) * 2;
      this.block(cx + jx, cy + jy, 4, color);
    }
  }

  /* ---------- vehicle icon (pixel cartoon, side/top hybrid) ---------- */
  function drawVehicleIcon(cv, veh) {
    const ctx = cv.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    const W = cv.width, H = cv.height;
    ctx.clearRect(0, 0, W, H);
    const c = veh.color;
    /* wheels */
    ctx.fillStyle = '#0a0a0c';
    ctx.fillRect(4, H - 14, 10, 12);
    ctx.fillRect(W - 14, H - 14, 10, 12);
    /* body */
    ctx.fillStyle = c;
    ctx.fillRect(6, H - 22, W - 12, 14);
    /* nose */
    ctx.fillRect(10, H - 28, W - 20, 8);
    /* dark trim */
    ctx.fillStyle = 'rgba(0,0,0,0.35)';
    ctx.fillRect(6, H - 22, W - 12, 3);
    /* cockpit */
    ctx.fillStyle = '#0b0b0d';
    ctx.fillRect(W / 2 - 8, H - 27, 16, 12);
    /* driver helmet */
    ctx.fillStyle = veh.color;
    ctx.fillRect(W / 2 - 4, H - 26, 8, 7);
    /* spotlights */
    ctx.fillStyle = C.COLORS.yellow;
    ctx.fillRect(11, H - 24, 4, 4);
    ctx.fillRect(W - 15, H - 24, 4, 4);
  }

  /* ---------- tiny track thumbnail ---------- */
  function drawThumb(cv, track, color) {
    const ctx = cv.getContext('2d');
    ctx.imageSmoothingEnabled = false;
    const W = cv.width, H = cv.height;
    ctx.fillStyle = C.COLORS.mapBg;
    ctx.fillRect(0, 0, W, H);
    if (!track) return;
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    for (const p of track.points) {
      if (p.x < minX) minX = p.x;
      if (p.x > maxX) maxX = p.x;
      if (p.y < minY) minY = p.y;
      if (p.y > maxY) maxY = p.y;
    }
    const pad = 4;
    const rx = maxX - minX || 1, ry = maxY - minY || 1;
    const s = Math.min((W - 2 * pad) / rx, (H - 2 * pad) / ry);
    const ox = (W - rx * s) / 2 - minX * s;
    const oy = (H - ry * s) / 2 - minY * s;
    ctx.fillStyle = color || C.COLORS.dim;
    for (let i = 0; i < track.N; i += 4) {
      const p = track.points[i];
      ctx.fillRect(Math.round(p.x * s + ox), Math.round(p.y * s + oy), 1, 1);
    }
  }

  /* ---------- G-force meter (g-g diagram) ---------- */
  function drawGmeter(ctx, W, H, lat, long, trail, maxG) {
    ctx.imageSmoothingEnabled = false;
    ctx.fillStyle = C.COLORS.mapBg;
    ctx.fillRect(0, 0, W, H);
    const cx = W / 2;
    const cy = H / 2;
    const R = Math.min(W, H) / 2 - 6;
    const mx = maxG || 2;
    /* rings */
    ctx.strokeStyle = C.COLORS.gmRing;
    ctx.lineWidth = 1;
    for (let g = 0.5; g <= mx; g += 0.5) {
      ctx.beginPath();
      ctx.arc(cx, cy, (g / mx) * R, 0, Math.PI * 2);
      ctx.stroke();
    }
    ctx.beginPath();
    ctx.moveTo(cx - R, cy);
    ctx.lineTo(cx + R, cy);
    ctx.moveTo(cx, cy - R);
    ctx.lineTo(cx, cy + R);
    ctx.stroke();
    const toXY = (l, n) => ({
      x: cx + (l / mx) * R,
      y: cy - (n / mx) * R,
    });
    /* trail */
    if (trail) {
      ctx.fillStyle = C.COLORS.gmTrail;
      for (const t of trail) {
        const q = toXY(t.l, t.n);
        ctx.fillRect(Math.round(q.x), Math.round(q.y), 1, 1);
      }
    }
    /* current */
    const q = toXY(lat, long);
    const mag = Math.hypot(lat, long);
    const col = mag >= 1 ? C.COLORS.red : mag >= 0.5 ? C.COLORS.yellow : C.COLORS.green;
    ctx.fillStyle = col;
    ctx.fillRect(Math.round(q.x) - 2, Math.round(q.y) - 2, 4, 4);
    ctx.fillStyle = C.COLORS.mapBg;
    ctx.fillRect(Math.round(q.x), Math.round(q.y), 1, 1);
    /* center dot */
    ctx.fillStyle = C.COLORS.gmCenter;
    ctx.fillRect(cx - 1, cy - 1, 2, 2);
    /* labels */
    ctx.fillStyle = C.COLORS.dim;
    ctx.font = "7px 'Press Start 2P', monospace";
    ctx.fillText('LAT', cx - R, cy - R + 8);
    ctx.fillText('LONG', cx + R - 26, cy + R + 0);
  }

  RC.TrackMap = TrackMap;
  RC.drawVehicleIcon = drawVehicleIcon;
  RC.drawThumb = drawThumb;
  RC.drawGmeter = drawGmeter;
})();
