/* Real-time navigation: fetch a pixel-style road-network tile around the
 * current GNSS position from RaceBrick_Sat2Pixel (POST /api/v1/nav,
 * format=pal16) in a background task, decode it and expose the latest frame. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NAV_IMG_SIZE 200

void nav_start(void);            /* allocate buffers + start worker task */
void nav_set_active(bool on);    /* enable fetching while the nav screen is open */
void nav_refresh(void);          /* request a new tile now */

/* status: 0 idle, 1 ok, 2 http error, 3 decode error, 4 no wifi, 5 no fix */
int  nav_status(void);
int  nav_frames(void);
int  nav_last_bytes(void);
int  nav_age_ms(void);

/* latest decoded frame; hold the lock only while blitting it */
const uint16_t *nav_frame_lock(int *w, int *h);
void nav_frame_unlock(void);
