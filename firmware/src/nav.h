/* Real-time navigation: fetch a pixel-style road-network tile around the
 * current GNSS position from RaceBrick_Sat2Pixel (POST /api/v1/nav,
 * format=pal16) in a background task, decode it and expose the latest frame. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NAV_IMG_SIZE 240      /* normal in-column square size, px */
#define NAV_FULL_W   320      /* fullscreen map (logical landscape panel) */
#define NAV_FULL_H   240
#define NAV_IMG_MAX_W 320     /* largest decoded frame (fullscreen) */
#define NAV_IMG_MAX_H 240

/* Zoom levels: index 0 = 4x, index 2 = 1x (base), last level = country view. */
#define NAV_ZOOM_DEFAULT 2

void nav_start(void);            /* allocate buffers + start worker task */
void nav_set_active(bool on);    /* enable fetching while the nav screen is open */
void nav_refresh(void);          /* request a new tile now */
float nav_heading(void);         /* smoothed heading used by the map (deg) */
void nav_set_mode(int mode, int sat_opacity);  /* 0=road 1=satellite 2=blend */
void nav_set_zoom_level(int level);  /* select zoom level (clamped) */
void nav_zoom_step(int dir);         /* +1 = zoom in, -1 = zoom out */
int  nav_zoom_level(void);           /* current zoom level index */
void nav_set_fullscreen(bool on);/* fetch 320x240 and use the whole page */
int  nav_http_code(void);        /* HTTP status of the last fetch (0 = comms) */

/* status: 0 idle, 1 ok, 2 http error, 3 decode error, 4 no wifi, 5 no fix */
int  nav_status(void);
int  nav_frames(void);
int  nav_last_bytes(void);
int  nav_age_ms(void);

/* latest decoded frame; hold the lock only while blitting it */
const uint16_t *nav_frame_lock(int *w, int *h);
void nav_frame_unlock(void);
