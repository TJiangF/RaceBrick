/* MPU6050 (6-axis IMU) simulator + GPS/IMU Kalman fusion.
 *
 * On the real board:
 *   RC.Mpu6050     -> I2C register reads (0x3B..0x48) + calibration offsets
 *   RC.KalmanFusion-> same algorithm in C, fed by UBX fixes + IMU samples
 * The API is intentionally identical so the UI/fusion layer does not change.
 */
(function () {
  const RC = window.RC;
  const C = RC.CONFIG;
  const PI = Math.PI;
  const G = 9.80665;

  /* ============================ MPU6050 ============================ */
  class Mpu6050 {
    constructor() {
      /* fixed manufacturing / mounting error recovered by calibration */
      const r = () => Math.random() * 2 - 1;
      this.mount = {
        ax: r() * 0.06,
        ay: r() * 0.06,
        az: r() * 0.03,
        gx: r() * 1.2,
        gy: r() * 1.2,
        gz: r() * 2.0,
      };
      this.bias = { ax: 0, ay: 0, az: 0, gx: 0, gy: 0, gz: 0 };
      this.calibrated = false;
      this.temp = 36.5 + r() * 0.8;
    }

    _n(v) {
      /* cheap gaussian-ish noise */
      return ((Math.random() + Math.random() + Math.random()) / 1.5 - 1) * v;
    }

    /* trueA: body acceleration (g) {ax,ay,az}; trueG: angular rate (deg/s) */
    sample(trueA, trueG) {
      const m = this.mount;
      const s = this.calibrated ? 1 : 0;
      const b = this.bias;
      return {
        ax: trueA.ax + m.ax - b.ax * s + this._n(C.IMU.ACC_NOISE),
        ay: trueA.ay + m.ay - b.ay * s + this._n(C.IMU.ACC_NOISE),
        az: trueA.az + m.az - b.az * s + this._n(C.IMU.ACC_NOISE),
        gx: trueG.gx + m.gx - b.gx * s + this._n(C.IMU.GYRO_NOISE),
        gy: trueG.gy + m.gy - b.gy * s + this._n(C.IMU.GYRO_NOISE),
        gz: trueG.gz + m.gz - b.gz * s + this._n(C.IMU.GYRO_NOISE),
      };
    }

    /* device at rest, flat on the table */
    sampleStatic() {
      return this.sample({ ax: 0, ay: 0, az: 1 }, { gx: 0, gy: 0, gz: 0 });
    }

    /* corrected (bias removed) readings */
    corrected(raw) {
      const b = this.bias;
      const s = this.calibrated ? 1 : 0;
      return {
        ax: raw.ax - b.ax * s,
        ay: raw.ay - b.ay * s,
        az: raw.az - b.az * s,
        gx: raw.gx - b.gx * s,
        gy: raw.gy - b.gy * s,
        gz: raw.gz - b.gz * s,
      };
    }

    applyCalibration(mean) {
      this.bias = { ax: mean.ax, ay: mean.ay, az: mean.az - 0, gx: mean.gx, gy: mean.gy, gz: mean.gz };
      /* az bias: we only care about the horizontal level, keep gravity on Z */
      this.bias.az = mean.az;
      this.calibrated = true;
    }
    clearCalibration() {
      this.calibrated = false;
    }
  }

  /* ======================= small matrix helpers ======================= */
  function eye(n) {
    const m = [];
    for (let i = 0; i < n; i++) {
      const row = new Array(n).fill(0);
      row[i] = 1;
      m.push(row);
    }
    return m;
  }
  function mul(A, B) {
    const n = A.length, m = B[0].length, k = B.length;
    const R = [];
    for (let i = 0; i < n; i++) {
      const row = new Array(m).fill(0);
      for (let j = 0; j < m; j++) {
        let s = 0;
        for (let p = 0; p < k; p++) s += A[i][p] * B[p][j];
        row[j] = s;
      }
      R.push(row);
    }
    return R;
  }
  function transpose(A) {
    const R = [];
    for (let j = 0; j < A[0].length; j++) {
      const row = [];
      for (let i = 0; i < A.length; i++) row.push(A[i][j]);
      R.push(row);
    }
    return R;
  }
  function add(A, B) {
    return A.map((r, i) => r.map((v, j) => v + B[i][j]));
  }
  function sub(A, B) {
    return A.map((r, i) => r.map((v, j) => v - B[i][j]));
  }
  function inv2(m) {
    const d = m[0][0] * m[1][1] - m[0][1] * m[1][0] || 1e-12;
    return [
      [m[1][1] / d, -m[0][1] / d],
      [-m[1][0] / d, m[0][0] / d],
    ];
  }

  /* ==================== GPS + IMU Kalman fusion ====================
   * State: [x, y, vx, vy] in local normalized map units.
   * Predict with body-frame acceleration rotated by the gyro heading,
   * correct with the GPS position fix.
   */
  class KalmanFusion {
    constructor() {
      this.reset();
    }
    reset() {
      /* state [x, y, vx, vy] in METERS (local frame) */
      this.x = [0, 0, 0, 0];
      this.P = [
        [4, 0, 0, 0],
        [0, 4, 0, 0],
        [0, 0, 1, 0],
        [0, 0, 0, 1],
      ];
      this.heading = 0; // math angle, forward = (cos, sin)
      this.headingSet = false;
      this.lastGps = null;
      this.inited = false;
      this.gpsCount = 0;
      this.sigma = 0; // residual (m) for display
      /* steady-state Kalman (alpha-beta) gains, tuned on the demo tracks */
      this.alpha = 0.5;
      this.beta = 0.15;
      this.accelWeight = 0.7; // IMU longitudinal accel trust
      this.lastFixDt = 0.125;
      this._fixTimer = 0;
    }
    seed(px, py) {
      this.x[0] = px;
      this.x[1] = py;
      this.x[2] = 0;
      this.x[3] = 0;
      this.P = [
        [2, 0, 0, 0],
        [0, 2, 0, 0],
        [0, 0, 1, 0],
        [0, 0, 0, 1],
      ];
      this.inited = true;
    }
    /* motion model: rotate the velocity vector by the gyro yaw (so the model
     * follows corners) then add longitudinal acceleration along the heading. */
    predict(dt, yawRate, aLong) {
      if (!this.inited) return;
      const x = this.x;
      const dy = yawRate * dt;
      const c = Math.cos(dy);
      const s = Math.sin(dy);
      let vx = c * x[2] - s * x[3];
      let vy = s * x[2] + c * x[3];
      const sp = Math.hypot(vx, vy);
      if (sp > 0.1) {
        vx += aLong * (vx / sp) * dt;
        vy += aLong * (vy / sp) * dt;
      }
      x[2] = vx;
      x[3] = vy;
      x[0] += vx * dt;
      x[1] += vy * dt;
    }

    /* measurement update with the steady-state Kalman gains (alpha-beta):
     *   alpha : position gain
     *   beta  : velocity gain  (per fix)
     * gamma : acceleration gain (per fix, set in update)
     */
    correct(zx, zy) {
      const rx = zx - this.x[0];
      const ry = zy - this.x[1];
      const dt = Math.max(this.lastFixDt || 0.125, 0.02);
      this.x[0] += this.alpha * rx;
      this.x[1] += this.alpha * ry;
      this.x[2] += (this.beta * rx) / dt;
      this.x[3] += (this.beta * ry) / dt;
      /* residual-based position sigma for the UI */
      const res = Math.hypot(rx, ry);
      this.sigma = this.sigma * 0.8 + res * 0.2;
    }
    /* one fusion step. axBody/ayBody in g, gzRad in rad/s, gps {x,y,r} in meters */
    update(dt, axBody, ayBody, gzRad, gps) {
      this._fixTimer += dt;
      if (this.headingSet) this.heading += gzRad * dt;
      const clampA = (v) => Math.max(-1.5, Math.min(1.5, v));
      const w = this.accelWeight;
      const aLong = this.headingSet ? clampA(axBody) * w * G : 0; // m/s^2 forward
      const yaw = this.headingSet ? gzRad : 0;
      this.predict(dt, yaw, aLong);

      if (gps) {
        if (!this.inited) {
          this.seed(gps.x, gps.y);
          this.lastGps = { x: gps.x, y: gps.y };
          this._fixTimer = 0;
          return;
        }
        if (this.lastGps) {
          const dx = gps.x - this.lastGps.x;
          const dy = gps.y - this.lastGps.y;
          if (Math.hypot(dx, dy) > 1.0) {
            const course = Math.atan2(dy, dx);
            if (!this.headingSet) {
              this.heading = course;
              this.headingSet = true;
            } else {
              let e = course - this.heading;
              while (e > PI) e -= 2 * PI;
              while (e < -PI) e += 2 * PI;
              this.heading += e * 0.15;
            }
          }
        }
        this.lastFixDt = this._fixTimer;
        this._fixTimer = 0;
        this.correct(gps.x, gps.y, gps.r);
        this.lastGps = { x: gps.x, y: gps.y };
        this.gpsCount++;
      }
    }
    get px() {
      return this.x[0];
    }
    get py() {
      return this.x[1];
    }
    get speed() {
      return Math.hypot(this.x[2], this.x[3]);
    }
  }

  RC.Mpu6050 = Mpu6050;
  RC.KalmanFusion = KalmanFusion;
})();
