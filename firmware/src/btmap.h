/* BLE phone-frame bridge (NavBridge / MotoNav-compatible UART service).
 * Receives type-5 320x240 paletted frames and exposes the latest one for the
 * PHONE page. Also advertises the service and sends U,1 / K,5,id / #G lines. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void btmap_start(void);              /* init NimBLE + buffers, if enabled */
void btmap_poll(void);               /* main-loop: ready/K/GPS notifications */
void btmap_set_enabled(bool on);     /* runtime BLE on/off (advertising) */
bool btmap_enabled(void);
bool btmap_connected(void);
bool btmap_subscribed(void);
const char *btmap_state(void);       /* OFF / ADVERTISING / CONNECTED / READY / STREAMING */

int  btmap_frames(void);
int  btmap_last_bytes(void);
int  btmap_age_ms(void);
bool btmap_has_frame(void);

/* Phone media control (sent as M,TOGGLE / M,NEXT / M,PREV notifications). */
void btmap_media_cmd(const char *cmd);
bool btmap_media_playing(void);

/* Navigation progress sent by the phone as "#P,remain_m,total_m"; -1 clears. */
bool btmap_nav_active(void);
int  btmap_nav_remain(void);
int  btmap_nav_total(void);

/* Latest decoded 320x240 RGB565 frame; hold the lock only while blitting. */
const uint16_t *btmap_frame_lock(int *w, int *h);
void btmap_frame_unlock(void);
