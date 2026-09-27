/* NEO-M9N GNSS receiver simulator.
 * Produces a believable acquisition sequence (NO FIX -> 2D -> 3D), a satellite
 * sky view (azimuth / elevation / C/N0) and a position solution.
 * On the real board this object is replaced by the u-blox UBX parser; the API
 * (update / sats / fix / hdop / lat / lon) stays the same.
 */
(function () {
  const RC = window.RC;

  const PRNS = [2, 5, 7, 9, 11, 13, 15, 17, 19, 20, 23, 24, 29, 30];

  class Gnss {
    constructor() {
      this.reset();
    }
    reset() {
      this.t = 0;
      this.fix = 'NO FIX';
      this.used = 0;
      this.visible = 0;
      this.hdop = 9.9;
      this.baseLat = 31.2304 + (Math.random() - 0.5) * 0.02;
      this.baseLon = 121.4737 + (Math.random() - 0.5) * 0.02;
      this.lat = this.baseLat;
      this.lon = this.baseLon;
      this.alt = 12.4;
      this.speed = 0;
      this.sats = PRNS.map((prn, i) => ({
        prn: prn,
        az: Math.random() * 360,
        el: 12 + Math.random() * 76,
        max: 33 + Math.random() * 17,
        start: 0.25 + i * 0.32,
        cn0: 0,
        used: false,
      }));
    }

    update(dt) {
      this.t += dt;
      let used = 0;
      let visible = 0;
      for (const s of this.sats) {
        s.az = (s.az + dt * 1.2) % 360;
        s.el = Math.max(4, Math.min(88, s.el + Math.sin(this.t * 0.18 + s.prn) * dt * 0.5));
        const ramp = Math.max(0, Math.min(1, (this.t - s.start) / 1.8));
        const env = ramp * (0.82 + 0.18 * Math.sin(this.t * 1.7 + s.prn * 2.1));
        s.cn0 = Math.max(0, s.max * env);
        s.used = s.cn0 >= 34 && s.el >= 10;
        if (s.cn0 > 0.5) visible++;
        if (s.used) used++;
      }
      this.used = used;
      this.visible = visible;
      this.hdop = Math.max(0.6, Math.min(9.9, 3.6 - used * 0.18 + Math.sin(this.t * 0.7) * 0.25));
      if (this.t > 1.2 && used >= 4) this.fix = '3D FIX';
      else if (this.t > 1.2 && used >= 3) this.fix = '2D FIX';
      else this.fix = 'NO FIX';
      const j = this.hdop * 3.2e-6;
      this.lat = this.baseLat + Math.sin(this.t * 0.9) * j;
      this.lon = this.baseLon + Math.cos(this.t * 0.7) * j;
      this.alt = 12.4 + Math.sin(this.t * 0.5) * 0.8;
      this.speed = this.fix === 'NO FIX' ? 0 : 0.6 + Math.abs(Math.sin(this.t * 0.3)) * 0.7;
    }

    fmtLat() {
      return this.lat.toFixed(6) + '° N';
    }
    fmtLon() {
      return this.lon.toFixed(6) + '° E';
    }
  }

  RC.Gnss = Gnss;
})();
