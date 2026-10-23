/* NEO-M9N GPS: UART NMEA reader for bring-up / debugging.
 * Production board: GPS-TX -> ESP GPIO11 (RX), GPS-RX -> ESP GPIO12 (TX).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define GPS_NSAT 64

typedef struct {
    char sys;     /* P=GPS B=BeiDou A=Galileo L=GLONASS Q=QZSS */
    int  prn;
    int  el;      /* elevation deg */
    int  az;      /* azimuth deg */
    int  snr;     /* C/N0 dBHz */
    bool used;    /* listed in GSA (or UBX svUsed) */
    bool seen;
    bool eph;     /* ephemeris available (UBX-NAV-SAT) */
    int  orbit;   /* orbit source (0 none,1 eph,2 almanac,3/4 AssistNow) */
    bool ubx;     /* updated by UBX-NAV-SAT */
} gps_sat_t;

typedef struct {
    bool   inited;
    int    source;        /* 0 = UART, 1 = I2C */
    int    baud;
    int    i2c_found;     /* number of devices seen on the I2C bus */
    uint8_t i2c_addrs[8];
    uint32_t bytes_rx;
    uint32_t sentences;
    uint32_t crc_errors;
    uint32_t gll_count;
    uint64_t last_rx_us;

    int    fix_type;      /* 0 none, 1 = 2D, 2 = 3D (from GSA), -1 unknown */
    int    fix_quality;   /* from GGA */
    int    sats_used;
    int    sats_visible;
    float  hdop;
    double lat, lon;
    float  alt;
    bool   has_pos;
    bool   has_time;      /* received a valid UTC time from GGA/RMC */
    float  speed_kmh;
    float  course;        /* course over ground, deg (0=N, clockwise) from RMC */
    int    utc_h, utc_m, utc_s;
    int    day, month, year;

    gps_sat_t sats[GPS_NSAT];
    int    nsat;

    char   last_line[100];
    char   last_gga[100];
    char   last_gsa[100];
    char   raw[8][100];   /* recent NMEA lines, newest at raw[0] */
    int    raw_n;

    char   last_txt[80];  /* last $GNTXT / $GNTXT message */
    bool   txt_toostrong; /* saw "RF input signal too strong" */

    /* UBX diagnostics */
    bool   have_ver;
    char   ver[32];
    bool   have_navsat;
    bool   have_monhw;
    int    ant_status;    /* 0 INIT 1 DONTKNOW 2 OK 3 SHORT 4 OPEN */
    int    ant_power;     /* 0 OFF 1 ON 2 DONTKNOW */
    int    jam_ind;       /* jamming indicator 0..255 */
    int    jam_state;     /* 0 unknown 1 ok 2 warning 3 critical */
} gps_state_t;

void gps_begin(void);           /* init UART at default baud */
void gps_begin_uart_pins(int rx, int tx); /* UART with explicit ESP rx/tx pins */
void gps_begin_i2c(void);       /* use the I2C (DDC) transport @0x42 */
void gps_toggle_source(void);   /* switch UART <-> I2C */
void gps_next_baud(void);       /* switch to the next candidate baud (UART) */
void gps_scan_i2c(void);        /* rescan the I2C bus */
void gps_cfg_gps_bds(void);     /* UBX-CFG-GNSS: enable only GPS + BeiDou */
void gps_disable_gll(void);     /* UBX-CFG-MSG: stop GLL output (TX test) */
void gps_restart(void);         /* UBX-CFG-RST: force a (cold) GNSS (re)start */
void gps_set_rate(int hz);      /* 1/5/10/25 Hz output rate (UART; finds module) */
void gps_set_nav_rate(int hz);  /* light-weight rate change (no baud scan) */

/* time pulse (TP / 1PPS) capture on PIN_GPS_TP */
bool     gps_pps_ok(void);      /* pulses seen recently */
uint32_t gps_pps_count(void);
int      gps_pps_period_ms(void);
void gps_poll(void);            /* read + parse available bytes */
void gps_dump(void);            /* log a one-line live summary + raw + sats */
gps_state_t *gps_get(void);
