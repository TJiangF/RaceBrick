/* Application shell + shared state. Mirrors js/app.js. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "sim.h"
#include "map.h"
#include "theme.h"
#include "input.h"

typedef enum {
    SC_IMU_CALIB, SC_HOME, SC_TRACK_EDIT, SC_SETTINGS, SC_MODE_SELECT, SC_MOUNT_SELECT,
    SC_NEW_TRACK_MARK_START, SC_RACING, SC_RACING_RECORDED, SC_LAP_SELECT, SC_SECTION_MARKING,
    SC_VEHICLE_SELECT, SC_TRACK_NAMING, SC_TRACK_LIST, SC_HISTORY, SC_HISTORY_DETAIL,
    SC_GPS_DEBUG, SC_LOCATION, SC_SOUND, SC_CONFIRM, SC_COUNT
} scid_t;

typedef void (*action_fn)(void);

typedef struct {
    char  trackName[12];
    char  date[20];
    int   mode;                 /* 0 new, 1 recorded */
    float duration;
    int   lapCount;
    float lapTime[HISTORY_LAPS];
    int   lapIndex[HISTORY_LAPS];
    float bestTime; int bestIndex; bool hasBest;
    int   vehicle;
    int   nsec;
    float sections[MAX_SECTIONS];
    float secBest[MAX_SECTIONS];
    int   secBestLap[MAX_SECTIONS];
    bool  secValid[MAX_SECTIONS];
    pt_t  trace[TRACE_MAX];
    int   nTrace;
} histrec_t;

typedef struct {
    /* ---- global ---- */
    int homeIndex;
    track_t tracks[MAX_TRACKS]; int ntracks;
    histrec_t log[HISTORY_RECORDS]; int nlog;
    engine_t engine; bool engine_valid;
    gnss_t gnss;
    mpu_t imu;
    int race_mount;
    int theme_light;
    scid_t raceTarget, raceBack;
    track_t sessionTrack; bool sessionTrack_valid;
    float sessionSections[MAX_SECTIONS]; int session_nsec;
    int sessionVehicle;
    float simSpeed;

    /* ---- navigation ---- */
    scid_t cur;
    bool overlay_active;
    scid_t overlay;
    int dir;

    /* ---- chrome ---- */
    char status[80];
    uint16_t statusColor;
    int leds[LED_COUNT];
    char toastMsg[48]; float toastT;

    /* ---- per-screen scratch ---- */
    int sel, cursor, entered;
    int set_brightness, set_ledcolor, set_ledbright; bool set_wifi, set_bt;
    int set_sound_vol; bool set_snd_confirm, set_snd_calib, set_snd_fix;
    int prev_fix;              /* last GPS fix_type, for the 3D-fix sound */
    bool marked; float postTimer;
    int panel;
    int dbg_page;              /* GPS debug sub-page: 0 = live NMEA, 1 = satellites */
    bool lapChecked[MAX_LAPS];

    float sec_sections[MAX_SECTIONS]; int sec_n; float sec_prev; float sec_pos; float sec_cand; bool sec_done;

    char avail[27]; int navail;

    int hist_sel; bool hist_entered;
    int hist_rec;              /* index into g.log */

    int imu_phase;             /* 0 sampling, 1 done */
    imu6_t imu_samples[100]; int imu_ns;
    float imu_progress; bool imu_stable; float imu_doneTimer;
    imu6_t imu_mean;

    char confirm_text[96]; char confirm_yes[8]; char confirm_no[8]; int confirm_sel;
    action_fn confirm_on_yes, confirm_on_no;

    /* ---- timing ---- */
    float now;
    float screenT;
    scid_t prev_cur;

    uint32_t seed;
    int demoCycle;
} app_t;

extern app_t g;

void app_go(scid_t id, int dir);
void app_go_home(int delta);
void app_go_home_to(int idx);
void app_toast(const char *msg);
void app_push_confirm(const char *text, const char *yes, const char *no,
                      action_fn on_yes, action_fn on_no);
void app_pop_overlay(void);
void app_record_race(int mode);
void app_save_settings(void);

void screens_enter(scid_t id);
void screens_handle(const event_t *ev);
void screens_handle_racing_common(const event_t *ev);
void screens_update(float dt);
void screens_render(void);
void screens_chrome(void);

void app_init(void);
void app_dispatch(const event_t *ev);
void app_update(float dt);
void app_render(void);
void app_autopilot(float dt);
