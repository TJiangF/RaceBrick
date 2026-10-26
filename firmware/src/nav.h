/* Real-time navigation: fetch a pixel-style road-network tile around the
 * current GNSS position from RaceBrick_Sat2Pixel (POST /api/v1/nav,
 * format=pal16) in a background task, decode it and expose the latest frame. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NAV_IMG_SIZE 240      /* displayed/downloaded square size, px */

/* Relative zoom applied to the base ground span (NAV_SIZE_M in nav.c).
 * 100 = base, >100 = zoom in. Range-limited so the server never gets asked
 * for an absurd ground scale. */
#define NAV_ZOOM_MIN_PCT 50
#define NAV_ZOOM_MAX_PCT 400
#define NAV_ZOOM_STEP_PCT 50

void nav_start(void);            /* allocate buffers + start worker task */
void nav_set_active(bool on);    /* enable fetching while the nav screen is open */
void nav_refresh(void);          /* request a new tile now */
float nav_heading(void);         /* smoothed heading used by the map (deg) */
void nav_set_mode(int mode, int sat_opacity);  /* 0=road 1=satellite 2=blend */
void nav_set_zoom(int pct);      /* zoom in/out, clamped to NAV_ZOOM_MIN/MAX */
int  nav_zoom_pct(void);         /* current zoom (100 = base) */
int  nav_http_code(void);        /* HTTP status of the last fetch (0 = comms) */

/* status: 0 idle, 1 ok, 2 http error, 3 decode error, 4 no wifi, 5 no fix */
int  nav_status(void);
int  nav_frames(void);
int  nav_last_bytes(void);
int  nav_age_ms(void);

/* latest decoded frame; hold the lock only while blitting it */
const uint16_t *nav_frame_lock(int *w, int *h);
void nav_frame_unlock(void);
