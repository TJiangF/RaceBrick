#include "sim.h"
#include "util.h"
#include "geo.h"
#include <math.h>
#include <string.h>

const int TRACK_DEF_COUNT = 3;

#define DENSE_STEP 24
#define MAX_DENSE  512
static float s_cum[MAX_DENSE + 1];

/* ---------------- tiny math helpers ---------------- */
static float wrap_pi(float d) {
    while (d > (float)M_PI) d -= 2.0f * (float)M_PI;
    while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
    return d;
}

/* ---------------- Catmull-Rom closed spline ---------------- */
static pt_t catmull(pt_t p0, pt_t p1, pt_t p2, pt_t p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    pt_t r;
    r.x = 0.5f * (2 * p1.x + (-p0.x + p2.x) * t +
                  (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 +
                  (-p0.x + 3 * p1.x - 3 * p2.x + p3.x) * t3);
    r.y = 0.5f * (2 * p1.y + (-p0.y + p2.y) * t +
                  (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 +
                  (-p0.y + 3 * p1.y - 3 * p2.y + p3.y) * t3);
    return r;
}

static int catmull_dense(const pt_t *ctrl, int m, pt_t *out) {
    int n = 0;
    for (int i = 0; i < m; i++) {
        pt_t p0 = ctrl[(i - 1 + m) % m];
        pt_t p1 = ctrl[i];
        pt_t p2 = ctrl[(i + 1) % m];
        pt_t p3 = ctrl[(i + 2) % m];
        for (int j = 0; j < DENSE_STEP; j++)
            out[n++] = catmull(p0, p1, p2, p3, (float)j / DENSE_STEP);
    }
    return n;
}

/* resample a closed polyline to N equal-arc-length points */
static void resample_closed(const pt_t *dense, int m, int N, pt_t *out) {
    s_cum[0] = 0;
    for (int i = 1; i <= m; i++) {
        pt_t a = dense[i - 1];
        pt_t b = dense[i % m];
        s_cum[i] = s_cum[i - 1] + hypotf(b.x - a.x, b.y - a.y);
    }
    float total = s_cum[m];
    if (total < 1e-9f) total = 1e-9f;
    int k = 0;
    for (int i = 0; i < N; i++) {
        float target = (total * i) / N;
        while (k < m - 1 && s_cum[k + 1] < target) k++;
        float segLen = s_cum[k + 1] - s_cum[k];
        if (segLen < 1e-9f) segLen = 1e-9f;
        float t = (target - s_cum[k]) / segLen;
        pt_t a = dense[k % m];
        pt_t b = dense[(k + 1) % m];
        out[i].x = a.x + (b.x - a.x) * t;
        out[i].y = a.y + (b.y - a.y) * t;
    }
}

static void normalize_ex(pt_t *pts, int N, float *pScale, float *pOx, float *pOy) {
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (int i = 0; i < N; i++) {
        if (pts[i].x < minX) minX = pts[i].x;
        if (pts[i].x > maxX) maxX = pts[i].x;
        if (pts[i].y < minY) minY = pts[i].y;
        if (pts[i].y > maxY) maxY = pts[i].y;
    }
    float rx = maxX - minX; if (rx == 0) rx = 1;
    float ry = maxY - minY; if (ry == 0) ry = 1;
    float scale = 0.92f / rx;
    float s2 = 0.92f / ry;
    if (s2 < scale) scale = s2;
    float ox = (1 - rx * scale) / 2 - minX * scale;
    float oy = (1 - ry * scale) / 2 - minY * scale;
    if (pScale) *pScale = scale;
    if (pOx) *pOx = ox;
    if (pOy) *pOy = oy;
    for (int i = 0; i < N; i++) {
        pts[i].x = pts[i].x * scale + ox;
        pts[i].y = pts[i].y * scale + oy;
    }
}

static void normalize(pt_t *pts, int N) { normalize_ex(pts, N, NULL, NULL, NULL); }

void track_finalize(track_t *t) {
    int N = t->N;
    pt_t *pts = t->points;
    for (int i = 0; i < N; i++) {
        pt_t a = pts[(i - 1 + N) % N];
        pt_t b = pts[i];
        pt_t c = pts[(i + 1) % N];
        float v1x = b.x - a.x, v1y = b.y - a.y;
        float v2x = c.x - b.x, v2y = c.y - b.y;
        float d = atan2f(v2y, v2x) - atan2f(v1y, v1x);
        d = wrap_pi(d);
        float ds = (hypotf(v1x, v1y) + hypotf(v2x, v2y)) / 2;
        if (ds < 1e-6f) ds = 1e-6f;
        t->curvNorm[i] = fabsf(d) / ds;
    }
    float maxc = 1e-6f;
    for (int i = 0; i < N; i++) if (t->curvNorm[i] > maxc) maxc = t->curvNorm[i];
    float avg = 0;
    for (int i = 0; i < N; i++) {
        float kn = t->curvNorm[i] / maxc;
        t->curvNorm[i] = kn;
        float sf = 1.0f - 0.8f * kn;
        if (sf < 0.42f) sf = 0.42f;
        t->speedFactor[i] = sf;
        avg += sf;
    }
    avg /= N;
    t->vTop = N / (avg * t->lapTarget);
    t->accel = t->vTop / 1.1f;
    t->brake = t->vTop / 0.7f;
    float total = 0;
    for (int i = 0; i < N; i++) {
        pt_t a = pts[i];
        pt_t b = pts[(i + 1) % N];
        total += hypotf(b.x - a.x, b.y - a.y);
    }
    t->total = total;
}

/* ---------------- track definitions (js/track.js RC.TRACK_DEFS) ---------------- */
static const pt_t DEF_A[] = {
    {0.5f, 0.08f}, {0.84f, 0.19f}, {0.88f, 0.47f}, {0.62f, 0.53f}, {0.85f, 0.72f},
    {0.74f, 0.91f}, {0.4f, 0.9f}, {0.16f, 0.73f}, {0.18f, 0.4f}, {0.3f, 0.15f},
};
static const pt_t DEF_B[] = {
    {0.5f, 0.1f}, {0.81f, 0.26f}, {0.7f, 0.5f}, {0.86f, 0.7f}, {0.6f, 0.9f},
    {0.3f, 0.86f}, {0.15f, 0.6f}, {0.36f, 0.45f}, {0.2f, 0.24f},
};
static const pt_t DEF_C[] = {
    {0.5f, 0.12f}, {0.76f, 0.16f}, {0.89f, 0.4f}, {0.7f, 0.6f}, {0.82f, 0.85f},
    {0.5f, 0.92f}, {0.2f, 0.8f}, {0.12f, 0.5f}, {0.32f, 0.35f}, {0.25f, 0.2f},
};

void track_build_def(track_t *t, int idx) {
    static pt_t dense[MAX_DENSE];
    const pt_t *ctrl;
    int m;
    memset(t, 0, sizeof(*t));
    t->N = TRK_N;
    t->lapTarget = 9.5f;
    t->lengthM = 180;
    t->maxSpeed = 90;
    switch (idx) {
    case 0: ctrl = DEF_A; m = (int)(sizeof(DEF_A) / sizeof(pt_t)); strcpy(t->name, "A"); t->vehicle = 0; t->best = 11.842f; break;
    case 1: ctrl = DEF_B; m = (int)(sizeof(DEF_B) / sizeof(pt_t)); strcpy(t->name, "B"); t->vehicle = 3; t->best = 13.207f; break;
    default: ctrl = DEF_C; m = (int)(sizeof(DEF_C) / sizeof(pt_t)); strcpy(t->name, "C"); t->vehicle = 1; t->best = 10.554f; break;
    }
    t->hasBest = true;
    int dn = catmull_dense(ctrl, m, dense);
    resample_closed(dense, dn, TRK_N, t->points);
    normalize(t->points, TRK_N);
    track_finalize(t);
}

void track_build_from_points(track_t *t, const pt_t *pts, int n, const char *name, int vehicle) {
    memset(t, 0, sizeof(*t));
    t->N = TRK_N;
    t->lapTarget = 9.5f;
    t->lengthM = 180;
    t->maxSpeed = 90;
    t->vehicle = vehicle;
    t->hasBest = false;
    strncpy(t->name, name, sizeof(t->name) - 1);
    resample_closed(pts, n, TRK_N, t->points);
    normalize(t->points, TRK_N);
    track_finalize(t);
}

void track_build_geo(track_t *t, const pt_t *mpts, int n, double lat0, double lon0,
                     const char *name, int vehicle) {
    memset(t, 0, sizeof(*t));
    t->N = TRK_N;
    t->lapTarget = 30.0f;
    t->vehicle = vehicle;
    t->hasBest = false;
    strncpy(t->name, name, sizeof(t->name) - 1);
    /* real perimeter in metres (closed loop) */
    float per = 0;
    for (int i = 0; i < n; i++) {
        pt_t a = mpts[i], b = mpts[(i + 1) % n];
        per += hypotf(b.x - a.x, b.y - a.y);
    }
    t->lengthM = per > 1.0f ? per : 1.0f;
    /* forward heading at the start (a few points in, to reduce noise) */
    int k = n > 5 ? 5 : (n > 1 ? 1 : 0);
    if (k) {
        float dx = mpts[k].x - mpts[0].x, dy = mpts[k].y - mpts[0].y;
        if (hypotf(dx, dy) > 0.5f) t->start_hdg = atan2f(dy, dx);
    }
    resample_closed(mpts, n, TRK_N, t->points);
    normalize_ex(t->points, TRK_N, &t->nscale, &t->nox, &t->noy);
    t->geo = true; t->glat0 = lat0; t->glon0 = lon0;
    track_finalize(t);
}

pt_t track_geo_to_norm(const track_t *t, double lat, double lon) {
    geo_ref_t r = { t->glat0, t->glon0 };
    float mx, my;
    geo_meters(&r, lat, lon, &mx, &my);
    pt_t q = { mx * t->nscale + t->nox, my * t->nscale + t->noy };
    return q;
}

/* nearest point on the closed polyline: progress in samples [0,N) + distance */
bool track_project(const track_t *t, pt_t q, float *s, float *dist) {
    int N = t->N;
    float best = 1e30f;
    int bi = -1;
    float bt = 0;
    for (int i = 0; i < N; i++) {
        pt_t a = t->points[i], b = t->points[(i + 1) % N];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len2 = dx * dx + dy * dy;
        if (len2 < 1e-9f) continue;
        float tt = ((q.x - a.x) * dx + (q.y - a.y) * dy) / len2;
        if (tt < 0) tt = 0;
        if (tt > 1) tt = 1;
        float px = a.x + dx * tt, py = a.y + dy * tt;
        float d2 = (q.x - px) * (q.x - px) + (q.y - py) * (q.y - py);
        if (d2 < best) { best = d2; bi = i; bt = tt; }
    }
    if (bi < 0) return false;
    if (s) *s = (float)bi + bt;
    if (dist) *dist = sqrtf(best);
    return true;
}

pt_t track_point_at(const track_t *t, float f) {
    int N = t->N;
    f = fmodf(f, 1.0f);
    if (f < 0) f += 1.0f;
    float i = f * N;
    int i0 = (int)floorf(i) % N;
    int i1 = (i0 + 1) % N;
    float tt = i - floorf(i);
    pt_t a = t->points[i0], b = t->points[i1];
    pt_t r = {a.x + (b.x - a.x) * tt, a.y + (b.y - a.y) * tt};
    return r;
}

pt_t track_tangent_at(const track_t *t, float f) {
    pt_t a = track_point_at(t, f - 0.004f);
    pt_t b = track_point_at(t, f + 0.004f);
    float dx = b.x - a.x, dy = b.y - a.y;
    float len = hypotf(dx, dy);
    if (len < 1e-6f) len = 1e-6f;
    pt_t r = {dx / len, dy / len};
    return r;
}

/* ================= MPU6050 ================= */
static float noise(float v) {
    return ((u_randf() + u_randf() + u_randf()) / 1.5f - 1.0f) * v;
}

void mpu_init(mpu_t *m) {
    m->mount_ax = u_rand_pm() * 0.06f;
    m->mount_ay = u_rand_pm() * 0.06f;
    m->mount_az = u_rand_pm() * 0.03f;
    m->mount_gx = u_rand_pm() * 1.2f;
    m->mount_gy = u_rand_pm() * 1.2f;
    m->mount_gz = u_rand_pm() * 2.0f;
    memset(&m->bias_ax, 0, sizeof(float) * 6);
    m->calibrated = false;
    m->temp = 36.5f + u_rand_pm() * 0.4f;
}

imu6_t mpu_sample(mpu_t *m, float ax, float ay, float az, float gx, float gy, float gz) {
    float s = m->calibrated ? 1.0f : 0.0f;
    imu6_t r;
    r.ax = ax + m->mount_ax - m->bias_ax * s + noise(IMU_ACC_NOISE);
    r.ay = ay + m->mount_ay - m->bias_ay * s + noise(IMU_ACC_NOISE);
    r.az = az + m->mount_az - m->bias_az * s + noise(IMU_ACC_NOISE);
    r.gx = gx + m->mount_gx - m->bias_gx * s + noise(IMU_GYRO_NOISE);
    r.gy = gy + m->mount_gy - m->bias_gy * s + noise(IMU_GYRO_NOISE);
    r.gz = gz + m->mount_gz - m->bias_gz * s + noise(IMU_GYRO_NOISE);
    return r;
}

imu6_t mpu_sample_static(mpu_t *m) {
    return mpu_sample(m, 0, 0, 1, 0, 0, 0);
}

imu6_t mpu_corrected(mpu_t *m, imu6_t raw) {
    float s = m->calibrated ? 1.0f : 0.0f;
    imu6_t r;
    r.ax = raw.ax - m->bias_ax * s;
    r.ay = raw.ay - m->bias_ay * s;
    r.az = raw.az - m->bias_az * s;
    r.gx = raw.gx - m->bias_gx * s;
    r.gy = raw.gy - m->bias_gy * s;
    r.gz = raw.gz - m->bias_gz * s;
    return r;
}

void mpu_apply_calibration(mpu_t *m, imu6_t mean) {
    m->bias_ax = mean.ax; m->bias_ay = mean.ay; m->bias_az = mean.az;
    m->bias_gx = mean.gx; m->bias_gy = mean.gy; m->bias_gz = mean.gz;
    m->calibrated = true;
}
void mpu_clear_calibration(mpu_t *m) { m->calibrated = false; }

/* ================= Kalman (alpha-beta) fusion ================= */
void fusion_reset(fusion_t *f) {
    memset(f->x, 0, sizeof(f->x));
    f->heading = 0;
    f->headingSet = false;
    f->hasLastGps = false;
    f->inited = false;
    f->gpsCount = 0;
    f->sigma = 0;
    f->alpha = 0.5f;
    f->beta = 0.15f;
    f->accelWeight = 0.7f;
    f->lastFixDt = 0.125f;
    f->fixTimer = 0;
}

void fusion_seed(fusion_t *f, float px, float py) {
    f->x[0] = px; f->x[1] = py; f->x[2] = 0; f->x[3] = 0;
    f->inited = true;
}

static void fusion_predict(fusion_t *f, float dt, float yawRate, float aLong) {
    if (!f->inited) return;
    float dy = yawRate * dt;
    float c = cosf(dy), s = sinf(dy);
    float vx = c * f->x[2] - s * f->x[3];
    float vy = s * f->x[2] + c * f->x[3];
    float sp = hypotf(vx, vy);
    if (sp > 0.1f) {
        vx += aLong * (vx / sp) * dt;
        vy += aLong * (vy / sp) * dt;
    }
    f->x[2] = vx; f->x[3] = vy;
    f->x[0] += vx * dt;
    f->x[1] += vy * dt;
}

static void fusion_correct(fusion_t *f, float zx, float zy) {
    float rx = zx - f->x[0];
    float ry = zy - f->x[1];
    float dt = f->lastFixDt > 0.02f ? f->lastFixDt : 0.02f;
    f->x[0] += f->alpha * rx;
    f->x[1] += f->alpha * ry;
    f->x[2] += (f->beta * rx) / dt;
    f->x[3] += (f->beta * ry) / dt;
    float res = hypotf(rx, ry);
    f->sigma = f->sigma * 0.8f + res * 0.2f;
}

void fusion_update(fusion_t *f, float dt, float axBody, float ayBody,
                   float gzRad, bool hasGps, float gx, float gy) {
    (void)ayBody;
    f->fixTimer += dt;
    if (f->headingSet) f->heading += gzRad * dt;
    float w = f->accelWeight;
    float aLong = 0;
    if (f->headingSet) {
        float a = axBody;
        if (a > 1.5f) a = 1.5f; else if (a < -1.5f) a = -1.5f;
        aLong = a * w * 9.80665f;
    }
    float yaw = f->headingSet ? gzRad : 0;
    fusion_predict(f, dt, yaw, aLong);

    if (hasGps) {
        if (!f->inited) {
            fusion_seed(f, gx, gy);
            f->lastGpsX = gx; f->lastGpsY = gy; f->hasLastGps = true;
            f->fixTimer = 0;
            return;
        }
        if (f->hasLastGps) {
            float dx = gx - f->lastGpsX;
            float dy = gy - f->lastGpsY;
            if (hypotf(dx, dy) > 1.0f) {
                float course = atan2f(dy, dx);
                if (!f->headingSet) { f->heading = course; f->headingSet = true; }
                else { float e = wrap_pi(course - f->heading); f->heading += e * 0.15f; }
            }
        }
        f->lastFixDt = f->fixTimer;
        f->fixTimer = 0;
        fusion_correct(f, gx, gy);
        f->lastGpsX = gx; f->lastGpsY = gy; f->hasLastGps = true;
        f->gpsCount++;
    }
}

float fusion_px(const fusion_t *f) { return f->x[0]; }
float fusion_py(const fusion_t *f) { return f->x[1]; }

/* ================= GNSS ================= */
static const int PRNS[GNSS_NSAT] = {2, 5, 7, 9, 11, 13, 15, 17, 19, 20, 23, 24, 29, 30};

void gnss_init(gnss_t *g) {
    g->t = 0;
    g->fix = 0;
    g->used = 0;
    g->visible = 0;
    g->hdop = 9.9f;
    g->baseLat = 31.2304 + (u_randf() - 0.5) * 0.02;
    g->baseLon = 121.4737 + (u_randf() - 0.5) * 0.02;
    g->lat = g->baseLat;
    g->lon = g->baseLon;
    g->alt = 12.4f;
    g->speed = 0;
    for (int i = 0; i < GNSS_NSAT; i++) {
        g->sats[i].prn = PRNS[i];
        g->sats[i].az = u_randf() * 360.0f;
        g->sats[i].el = 12.0f + u_randf() * 76.0f;
        g->sats[i].max = 33.0f + u_randf() * 17.0f;
        g->sats[i].start = 0.25f + i * 0.32f;
        g->sats[i].cn0 = 0;
        g->sats[i].used = false;
    }
}

void gnss_update(gnss_t *g, float dt) {
    g->t += dt;
    int used = 0, visible = 0;
    for (int i = 0; i < GNSS_NSAT; i++) {
        sat_t *s = &g->sats[i];
        s->az = fmodf(s->az + dt * 1.2f, 360.0f);
        s->el += sinf(g->t * 0.18f + s->prn) * dt * 0.5f;
        if (s->el < 4) s->el = 4;
        if (s->el > 88) s->el = 88;
        float ramp = (g->t - s->start) / 1.8f;
        if (ramp < 0) ramp = 0;
        if (ramp > 1) ramp = 1;
        float env = ramp * (0.82f + 0.18f * sinf(g->t * 1.7f + s->prn * 2.1f));
        s->cn0 = s->max * env;
        if (s->cn0 < 0) s->cn0 = 0;
        s->used = (s->cn0 >= 34.0f && s->el >= 10.0f);
        if (s->cn0 > 0.5f) visible++;
        if (s->used) used++;
    }
    g->used = used;
    g->visible = visible;
    float hd = 3.6f - used * 0.18f + sinf(g->t * 0.7f) * 0.25f;
    if (hd < 0.6f) hd = 0.6f;
    if (hd > 9.9f) hd = 9.9f;
    g->hdop = hd;
    if (g->t > 1.2f && used >= 4) g->fix = 2;
    else if (g->t > 1.2f && used >= 3) g->fix = 1;
    else g->fix = 0;
    double j = g->hdop * 3.2e-6;
    g->lat = g->baseLat + sin(g->t * 0.9) * j;
    g->lon = g->baseLon + cos(g->t * 0.7) * j;
    g->alt = 12.4f + sinf(g->t * 0.5f) * 0.8f;
    g->speed = (g->fix == 0) ? 0 : 0.6f + fabsf(sinf(g->t * 0.3f)) * 0.7f;
}

/* ================= RaceEngine ================= */
static pt_t engine_fused(const engine_t *e) {
    float L = e->track->lengthM;
    pt_t r = {fusion_px(&e->fusion) / L, fusion_py(&e->fusion) / L};
    return r;
}

static void engine_run_fusion(engine_t *e, float dt) {
    track_t *t = e->track;
    if (!e->useSensors) {
        pt_t p = track_point_at(t, e->s / t->N);
        fusion_seed(&e->fusion, p.x, p.y);
        return;
    }
    float prog = e->s / t->N;
    pt_t p = track_point_at(t, prog);
    pt_t tan = track_tangent_at(t, prog);
    float hMath = atan2f(tan.y, tan.x);
    float yaw = 0;
    if (e->has_ph) {
        float d = wrap_pi(hMath - e->ph);
        yaw = d / (dt > 1e-4f ? dt : 1e-4f);
    }
    e->ph = hMath; e->has_ph = true;
    float speedMs = (e->v / t->N) * t->lengthM;
    float prevSpeed = e->has_ps ? e->ps : speedMs;
    float longA = (speedMs - prevSpeed) / (dt > 1e-4f ? dt : 1e-4f);
    e->ps = speedMs; e->has_ps = true;
    const float GRIP = 2.0f * 9.80665f;
    float latA = speedMs * yaw;
    if (latA > GRIP) latA = GRIP;
    if (latA < -GRIP) latA = -GRIP;
    if (longA > GRIP) longA = GRIP;
    if (longA < -GRIP) longA = -GRIP;
    float axBody = longA / 9.80665f;
    float ayBody = latA / 9.80665f;

    imu6_t raw = mpu_sample(e->sensor, axBody, ayBody, 1, 0, 0, (yaw * 180.0f) / (float)M_PI);
    imu6_t corr = mpu_corrected(e->sensor, raw);
    float gzDeg = corr.gz;
    float axUse = corr.ax;
    float ayUse = corr.ay;
    if (e->mount == 1) {
        float tt = e->time;
        e->steerAngle = 0.7f * sinf(tt * 1.6f) + 0.3f * sinf(tt * 4.1f);
        float ddelta = 0.7f * 1.6f * cosf(tt * 1.6f) + 0.3f * 4.1f * cosf(tt * 4.1f);
        gzDeg += (ddelta * 180.0f) / (float)M_PI;
        ayUse += sinf(e->steerAngle);
    }

    e->gpsTimer += dt;
    bool hasGps = false;
    float gx = 0, gy = 0;
    float hz = GPS_HZ;
    if (e->gpsTimer >= 1.0f / hz) {
        e->gpsTimer = 0;
        if (u_randf() > GPS_DROPOUT) {
            float n = GPS_NOISE;
            gx = p.x + u_rand_pm() * n;
            gy = p.y + u_rand_pm() * n;
            hasGps = true;
            e->rawGpsX = gx; e->rawGpsY = gy;
            if (e->nGps < GPS_MAX) e->gpsLapTrace[e->nGps++] = (pt_t){gx, gy};
            e->gpsFix = true;
        } else {
            e->gpsFix = false;
        }
    }
    /* GPS fix is passed in the same metric frame as the Kalman state (meters) */
    fusion_update(&e->fusion, dt, axUse, ayUse, (gzDeg * (float)M_PI) / 180.0f,
                  hasGps, gx * t->lengthM, gy * t->lengthM);

    float k = 0.12f;
    e->gLat = e->gLat * (1 - k) + ayBody * k;
    e->gLong = e->gLong * (1 - k) + axBody * k;
    e->gTick = (e->gTick + 1) % 2;
    if (e->gTick == 0) {
        if (e->nGTrail < 70) e->gTrail[e->nGTrail++] = (gpt_t){e->gLat, e->gLong};
        else {
            memmove(&e->gTrail[0], &e->gTrail[1], sizeof(gpt_t) * 69);
            e->gTrail[69] = (gpt_t){e->gLat, e->gLong};
        }
    }
}

void engine_reset(engine_t *e) {
    track_t *t = e->track;
    e->v = t->vTop * 0.5f;
    e->s = 0.5f;
    e->lap = 1;
    e->elapsed = 0;
    e->time = 0;
    e->nLaps = 0;
    e->hasBestLap = false;
    e->bestTime = 0;
    e->bestIndex = 0;
    for (int i = 0; i < TRK_N; i++) e->currentTimes[i] = NAN;
    e->nTrace = 0;
    e->nGps = 0;
    e->diff = 0;
    for (int i = 0; i < LED_COUNT; i++) e->leds[i] = 0;
    e->paused = false;
    e->running = true;
    fusion_reset(&e->fusion);
    pt_t p0 = track_point_at(t, e->s / t->N);
    fusion_seed(&e->fusion, p0.x * t->lengthM, p0.y * t->lengthM);
    e->has_ph = false; e->has_ps = false;
    e->gpsTimer = 0; e->gTick = 0;
    e->gpsLapTrace[0] = p0; e->nGps = 1;
    e->gLat = 0; e->gLong = 0; e->nGTrail = 0;
    e->gpsFix = false;
}

void engine_init(engine_t *e, track_t *track, bool useHistBest, mpu_t *sensor) {
    memset(e, 0, sizeof(*e));
    e->track = track;
    e->traceEvery = 3;
    e->speedMul = 1;
    e->useSensors = true;
    e->mount = 0;
    e->sensor = sensor;
    e->hasHistBest = useHistBest;
    e->histBest = track->hasBest ? track->best : 0;
    engine_set_profile(e, 0);
    engine_reset(e);
}

void engine_set_profile(engine_t *e, int steering) {
    e->mount = steering;
    if (steering) {
        e->fusion.alpha = FUSION_ALPHA_STEER;
        e->fusion.beta = FUSION_BETA_STEER;
        e->fusion.accelWeight = FUSION_ACC_STEER;
    } else {
        e->fusion.alpha = FUSION_ALPHA_CHASSIS;
        e->fusion.beta = FUSION_BETA_CHASSIS;
        e->fusion.accelWeight = FUSION_ACC_CHASSIS;
    }
}

void engine_fill_times(engine_t *e) {
    float *t = e->currentTimes;
    int N = TRK_N;
    for (int i = 0; i < N; i++) {
        if (!isnan(t[i])) continue;
        int p = i - 1;
        while (p >= 0 && isnan(t[p])) p--;
        int n = i + 1;
        while (n < N && isnan(t[n])) n++;
        if (p >= 0 && n < N) t[i] = t[p] + ((t[n] - t[p]) * (i - p)) / (n - p);
        else if (p >= 0) t[i] = t[p];
        else if (n < N) t[i] = t[n];
        else t[i] = 0;
    }
}

void engine_complete_lap(engine_t *e) {
    float lapTime = e->elapsed;
    engine_fill_times(e);
    if (e->nLaps < MAX_LAPS) {
        lap_t *lap = &e->laps[e->nLaps];
        lap->index = e->nLaps + 1;
        lap->time = lapTime;
        memcpy(lap->times, e->currentTimes, sizeof(float) * TRK_N);
        lap->nTrace = e->nTrace;
        memcpy(lap->trace, e->lapTrace, sizeof(pt_t) * e->nTrace);
        lap->nGps = e->nGps;
        memcpy(lap->gps, e->gpsLapTrace, sizeof(pt_t) * e->nGps);
        lap->valid = lapTime > 1.0f;
        if (lap->valid && (!e->hasBestLap || lapTime < e->bestTime)) {
            e->hasBestLap = true;
            e->bestTime = lapTime;
            e->bestIndex = lap->index;
        }
        e->nLaps++;
    }
    e->elapsed = 0;
    e->lap++;
    for (int i = 0; i < TRK_N; i++) e->currentTimes[i] = NAN;
    e->nTrace = 0;
    e->nGps = 0;
}

static void engine_led_state(engine_t *e) {
    int *out = e->leds;
    for (int i = 0; i < LED_COUNT; i++) out[i] = 0;
    float diff = e->diff;
    if (isnan(diff) || fabsf(diff) < 0.02f) return;
    int n = (int)ceilf(fabsf(diff) / DIFF_PER_LED);
    if (n < 1) n = 1;
    if (n > MAX_LED) n = MAX_LED;
    if (diff > 0) {
        for (int i = 0; i < n; i++) { int k = 4 - i; if (k >= 0) out[k] = 1; }
    } else {
        for (int i = 0; i < n; i++) { int k = 5 + i; if (k < LED_COUNT) out[k] = 2; }
    }
}

static void engine_compute_diff(engine_t *e) {
    if (!e->hasBestLap) {
        e->diff = 0;
    } else {
        int idx = ((int)floorf(e->s)) % TRK_N;
        if (idx < 0) idx += TRK_N;
        float bt = e->laps[e->bestIndex - 1].times[idx];
        e->diff = isnan(bt) ? 0 : (e->elapsed - bt);
    }
    engine_led_state(e);
}

static void engine_step(engine_t *e, float dt) {
    int N = TRK_N;
    engine_run_fusion(e, dt);
    int idx = ((int)floorf(e->s)) % N;
    if (idx < 0) idx += N;
    float target = e->track->speedFactor[idx] * e->track->vTop;
    float rate = target > e->v ? e->track->accel : e->track->brake;
    float dv = target - e->v;
    float step = rate * dt;
    if (dv > step) dv = step; else if (dv < -step) dv = -step;
    e->v += dv;
    float prevS = e->s;
    e->s += e->v * dt;
    int from = (int)floorf(prevS);
    int to = (int)floorf(e->s);
    if (to > from) {
        pt_t fp = engine_fused(e);
        for (int j = from + 1; j <= to; j++) {
            int jj = ((j % N) + N) % N;
            e->currentTimes[jj] = e->elapsed;
            if (fmodf(jj, e->traceEvery) == 0 && e->nTrace < TRACE_MAX)
                e->lapTrace[e->nTrace++] = fp;
        }
    }
    if (e->s >= N) {
        e->s -= N;
        engine_complete_lap(e);
    }
}

void engine_update(engine_t *e, float dtSec) {
    if (e->paused || !e->running) return;
    dtSec *= e->speedMul;
    e->time += dtSec;
    e->elapsed += dtSec;
    float remaining = dtSec;
    int guard = 0;
    while (remaining > 0 && guard++ < 64) {
        float step = remaining < 0.03f ? remaining : 0.03f;
        engine_step(e, step);
        remaining -= step;
    }
    engine_compute_diff(e);
}

float engine_progress(const engine_t *e) { return e->s / e->track->N; }

float engine_speed_kmh(const engine_t *e) {
    return (e->v / e->track->N) * e->track->lengthM * 3.6f;
}
float engine_heading_deg(const engine_t *e) {
    pt_t t = track_tangent_at(e->track, engine_progress(e));
    float d = atan2f(t.x, -t.y) * 180.0f / (float)M_PI + 360.0f;
    return fmodf(d, 360.0f);
}
float engine_dist_m(const engine_t *e) {
    return engine_progress(e) * e->track->lengthM;
}
float engine_alt_m(const engine_t *e) {
    float p = engine_progress(e);
    return 84 + sinf(p * (float)M_PI * 4) * 6 + sinf(e->time * 0.4f) * 0.3f;
}
float engine_lat_g(const engine_t *e) {
    int idx = ((int)floorf(e->s)) % TRK_N;
    if (idx < 0) idx += TRK_N;
    float kn = e->track->curvNorm[idx];
    float sp = e->v / e->track->vTop;
    return kn * sp * sp * 2.2f;
}

void engine_section_deltas(const engine_t *e, float *out) {
    int n = e->track->nsec;
    for (int i = 0; i < n; i++) {
        float f = e->track->sections[i];
        if (!e->hasBestLap) { out[i] = NAN; continue; }
        int idx = (int)floorf(f * TRK_N);
        if (idx > TRK_N - 1) idx = TRK_N - 1;
        float bt = e->laps[e->bestIndex - 1].times[idx];
        float t = NAN;
        if (f <= engine_progress(e) + 0.001f) t = e->currentTimes[idx];
        out[i] = (!isnan(t) && !isnan(bt)) ? (t - bt) : NAN;
    }
}
