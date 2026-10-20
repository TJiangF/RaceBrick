/* Track geometry + race simulation + MPU6050/GNSS simulators.
 * Direct port of js/track.js, js/imu.js and js/gnss.js.
 */
#pragma once
#include <stdbool.h>
#include "config.h"

typedef struct { float x, y; } pt_t;
typedef struct { float l, n; } gpt_t;   /* lateral / longitudinal g (trail) */

/* ---------------- track ---------------- */
typedef struct {
    int   N;
    char  name[8];
    int   vehicle;
    float best;        /* historical best (s) */
    bool  hasBest;
    float lapTarget;
    float lengthM;
    float maxSpeed;
    pt_t  points[TRK_N];
    float curvNorm[TRK_N];
    float speedFactor[TRK_N];
    float total;
    float vTop, accel, brake;
    float sections[MAX_SECTIONS];
    int   nsec;

    /* real-GNSS geo reference (set for tracks recorded from GPS) */
    bool   geo;
    double glat0, glon0;        /* origin (first recorded point) */
    float  nscale, nox, noy;    /* metres -> normalised: norm = m*nscale + (nox,noy) */
    float  start_hdg;           /* forward heading at the recorded start (rad, metres frame) */
    /* start/finish line (editable, defaults to the recorded start) */
    double line_lat, line_lon;
    float  line_hdg;
    int    store_slot;          /* external-flash slot (-1 = not stored); runtime only */
} track_t;

void track_build_def(track_t *t, int defIndex);
void track_build_from_points(track_t *t, const pt_t *pts, int n, const char *name, int vehicle);
/* build from real-GNSS metre-space points, keeping the geo origin + transform */
void track_build_geo(track_t *t, const pt_t *mpts, int n, double lat0, double lon0,
                     const char *name, int vehicle);
pt_t track_geo_to_norm(const track_t *t, double lat, double lon);
bool track_project(const track_t *t, pt_t norm, float *s, float *dist);
void track_finalize(track_t *t);
pt_t track_point_at(const track_t *t, float f);
pt_t track_tangent_at(const track_t *t, float f);

/* ---------------- MPU6050 + fusion ---------------- */
typedef struct {
    float ax, ay, az, gx, gy, gz;
} imu6_t;

typedef struct {
    float mount_ax, mount_ay, mount_az, mount_gx, mount_gy, mount_gz;
    float bias_ax, bias_ay, bias_az, bias_gx, bias_gy, bias_gz;
    bool  calibrated;
    float temp;
} mpu_t;

void   mpu_init(mpu_t *m);
imu6_t mpu_sample(mpu_t *m, float ax, float ay, float az, float gx, float gy, float gz);
imu6_t mpu_sample_static(mpu_t *m);
imu6_t mpu_corrected(mpu_t *m, imu6_t raw);
void   mpu_apply_calibration(mpu_t *m, imu6_t mean);
void   mpu_clear_calibration(mpu_t *m);

typedef struct {
    float x[4];        /* x, y, vx, vy in meters */
    float heading;
    bool  headingSet;
    float lastGpsX, lastGpsY;
    bool  hasLastGps;
    bool  inited;
    int   gpsCount;
    float sigma;
    float alpha, beta, accelWeight;
    float lastFixDt;
    float fixTimer;
} fusion_t;

void  fusion_reset(fusion_t *f);
void  fusion_seed(fusion_t *f, float px, float py);
void  fusion_update(fusion_t *f, float dt, float axBody, float ayBody,
                    float gzRad, bool hasGps, float gx, float gy);
float fusion_px(const fusion_t *f);
float fusion_py(const fusion_t *f);

/* ---------------- GNSS ---------------- */
typedef struct {
    int   prn;
    float az, el, max, start, cn0;
    bool  used;
} sat_t;

#define GNSS_NSAT 14
typedef struct {
    float t;
    int   fix;        /* 0 NO FIX, 1 2D, 2 3D */
    int   used, visible;
    float hdop;
    double baseLat, baseLon, lat, lon;
    float alt, speed;
    sat_t sats[GNSS_NSAT];
} gnss_t;

void gnss_init(gnss_t *g);
void gnss_update(gnss_t *g, float dt);

/* ---------------- laps / engine ---------------- */
typedef struct {
    int   index;
    float time;
    float times[TRK_N];      /* NAN = missing */
    pt_t  trace[TRACE_MAX];
    int   nTrace;
    pt_t  gps[GPS_MAX];
    int   nGps;
    bool  valid;
} lap_t;

typedef struct {
    track_t *track;
    float v, s;
    int   lap;
    float elapsed, time;
    float bestTime;
    int   bestIndex;
    bool  hasBestLap;
    lap_t laps[MAX_LAPS];
    int   nLaps;
    float currentTimes[TRK_N];
    pt_t  lapTrace[TRACE_MAX];
    int   nTrace;
    pt_t  gpsLapTrace[GPS_MAX];
    int   nGps;
    float diff;
    int   leds[LED_COUNT];   /* 0 off, 1 red (slow), 2 green (fast) */
    bool  paused, running;
    bool  logged;

    mpu_t    *sensor;
    fusion_t  fusion;
    bool      useSensors;
    int       mount;         /* 0 chassis, 1 steering */
    float     steerAngle;
    bool      has_ph; float ph;
    bool      has_ps; float ps;
    float     gpsTimer;
    int       gTick;
    float     gLat, gLong;
    gpt_t     gTrail[70];
    int       nGTrail;
    bool      gpsFix;
    float     rawGpsX, rawGpsY;
    float     speedMul;
    float histBest;
    bool  hasHistBest;
    float traceEvery;

    /* real-GNSS mode */
    bool  real;         /* track->geo: drive from real GPS instead of the sim */
    bool  gnssInit;
    bool  lapArmed;
    float sPrev;
    float alongPrev;
} engine_t;

void  engine_init(engine_t *e, track_t *track, bool useHistBest, mpu_t *sensor);
void  engine_set_profile(engine_t *e, int steering);   /* 0 chassis, 1 steering */
void  engine_reset(engine_t *e);
void  engine_update(engine_t *e, float dt);
/* real-GNSS racing: project the live fix onto the track, time laps */
void  engine_update_gnss(engine_t *e, float dt, double lat, double lon,
                         float speed_kmh, bool valid);
float engine_speed_kmh(const engine_t *e);
float engine_heading_deg(const engine_t *e);
float engine_dist_m(const engine_t *e);
float engine_alt_m(const engine_t *e);
float engine_lat_g(const engine_t *e);
float engine_progress(const engine_t *e);

/* fill out[i] = split delta at sections[i] (NAN when unknown) */
void  engine_section_deltas(const engine_t *e, float *out);

extern const int TRACK_DEF_COUNT;
