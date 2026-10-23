/* Phone-assisted WiFi provisioning: the ESP32 starts a SoftAP and a tiny web
 * page. Connect the phone to the AP, open http://192.168.4.1, enter the home
 * WiFi SSID/password; it is saved to NVS and the STA connects. */
#pragma once
#include <stdbool.h>

void netcfg_start_ap(void);      /* idempotent: start AP + web server */
bool netcfg_running(void);
bool netcfg_connected(void);
const char *netcfg_ip(void);
const char *netcfg_ap_ssid(void);
void netcfg_status(char *buf, int cap);

/* Remote commands from the phone, polled by the main loop:
 * 1 = HOME, 2 = PERFORMANCE, 3 = START RACE, 0 = none. */
int  netcfg_take_cmd(void);
