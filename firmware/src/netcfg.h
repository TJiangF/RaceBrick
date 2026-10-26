/* Phone-assisted WiFi provisioning: the ESP32 starts a SoftAP and a tiny web
 * page. Connect the phone to the AP, open http://192.168.4.1, enter the home
 * WiFi SSID/password; it is saved to NVS and the STA connects. */
#pragma once
#include <stdbool.h>

void netcfg_start_ap(void);      /* idempotent: start AP + web server */
bool netcfg_has_creds(void);     /* saved home-WiFi credentials present */
bool netcfg_running(void);
bool netcfg_connected(void);
const char *netcfg_ip(void);
const char *netcfg_ap_ssid(void);
void netcfg_status(char *buf, int cap);
/* Disable/enable WiFi modem sleep for low-latency frame fetch (nav screen). */
void netcfg_set_low_latency(bool on);

/* Remote commands from the phone, polled by the main loop:
 * 1 = HOME, 2 = PERFORMANCE, 3 = START RACE, 4 = GPS DEBUG, 5 = NAV, 0 = none. */
int  netcfg_take_cmd(void);

/* Debug: jump to any screen id (see scid_t). Returns -1 when none pending. */
int  netcfg_take_goto(int *enter);

/* Debug: inject a short button press for bench automation.
 * Returns 0=none, 1=UP, 2=DOWN, 3=PUSH, 4=BACK. */
int  netcfg_take_key(void);
