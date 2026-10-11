#include "gps.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "gps";

#define GPS_UART  UART_NUM_1
#define NEO_ADDR  0x42      /* u-blox DDC / I2C address */

/* u-blox M9 default UART1 baud is 38400 (M8 was 9600) */
static const int BAUDS[] = {38400, 9600, 115200, 57600, 230400, 4800, 460800, 921600};
static const int NBAUD = (int)(sizeof(BAUDS) / sizeof(BAUDS[0]));
static int s_baud_idx = 0;

static gps_state_t s_st;
static bool s_uart_ready = false;

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_idev = NULL;

static bool ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, int n);

static char s_line[128];
static int  s_ln = 0;

gps_state_t *gps_get(void) { return &s_st; }

static void reset_counters(void) {
    s_st.bytes_rx = 0;
    s_st.sentences = 0;
    s_st.crc_errors = 0;
    s_st.last_rx_us = 0;
    s_ln = 0;
}

/* ================= NMEA parsing ================= */
static double nmea_deg(const char *dm, const char *hemi) {
    if (!dm || !*dm) return 0;
    double v = atof(dm);
    int deg = (int)(v / 100);
    double min = v - deg * 100.0;
    double d = deg + min / 60.0;
    if (hemi && (*hemi == 'S' || *hemi == 'W')) d = -d;
    return d;
}

static gps_sat_t *sat_find_sys(char sys, int prn) {
    for (int i = 0; i < s_st.nsat; i++)
        if (s_st.sats[i].prn == prn && s_st.sats[i].sys == sys) return &s_st.sats[i];
    if (s_st.nsat < GPS_NSAT) {
        gps_sat_t *s = &s_st.sats[s_st.nsat++];
        memset(s, 0, sizeof(*s));
        s->prn = prn;
        s->sys = sys;
        return s;
    }
    return NULL;
}
static gps_sat_t *sat_find_prn(int prn) {
    for (int i = 0; i < s_st.nsat; i++) if (s_st.sats[i].prn == prn) return &s_st.sats[i];
    return NULL;
}

static void push_raw(const char *line) {
    for (int i = 7; i >= 1; i--) memcpy(s_st.raw[i], s_st.raw[i - 1], sizeof(s_st.raw[i]));
    strncpy(s_st.raw[0], line, sizeof(s_st.raw[0]) - 1);
    s_st.raw[0][sizeof(s_st.raw[0]) - 1] = 0;
    if (s_st.raw_n < 8) s_st.raw_n++;
}

static int parse_fields(char *line, char **f, int maxf) {
    int n = 0;
    char *p = line;
    if (*p == '$') p++;
    f[n++] = p;
    while (*p && n < maxf) {
        if (*p == ',' || *p == '*') {
            bool star = (*p == '*');
            *p = 0;
            if (star) break;
            f[n++] = p + 1;
        }
        p++;
    }
    return n;
}

static bool check_crc(const char *line) {
    const char *star = strchr(line, '*');
    if (!star) return false;
    uint8_t c = 0;
    for (const char *p = line + 1; p < star; p++) c ^= (uint8_t)*p;
    unsigned got = 0;
    if (sscanf(star + 1, "%x", &got) != 1) return false;
    return (uint8_t)got == c;
}

static void handle_line(const char *raw) {
    strncpy(s_st.last_line, raw, sizeof(s_st.last_line) - 1);
    s_st.last_line[sizeof(s_st.last_line) - 1] = 0;
    push_raw(raw);

    if (raw[0] != '$') return;
    if (!check_crc(raw)) { s_st.crc_errors++; return; }
    s_st.sentences++;

    char buf[128];
    strncpy(buf, raw, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *f[24];
    int nf = parse_fields(buf, f, 24);
    if (nf < 2) return;
    const char *id = f[0];
    size_t idlen = strlen(id);
    if (idlen < 5) return;               /* e.g. "GNGGA" / "GBGSV" = 5 chars */
    const char *type = id + idlen - 3;   /* "GGA" / "GSV" / ... */

    if (!strcmp(type, "GGA")) {
        snprintf(s_st.last_gga, sizeof(s_st.last_gga), "%s", raw);
        if (nf > 8) {
            if (f[1][0]) {
                s_st.has_time = true;
                s_st.utc_h = atoi(f[1]); s_st.utc_m = atoi(f[1] + 2); s_st.utc_s = atoi(f[1] + 4);
            }
            if (f[2][0]) { s_st.lat = nmea_deg(f[2], f[3]); s_st.has_pos = true; }
            if (f[4][0]) s_st.lon = nmea_deg(f[4], f[5]);
            int q = atoi(f[6]);
            s_st.fix_quality = q;
            int ns = atoi(f[7]);
            if (q > 0 && ns > 0) s_st.sats_used = ns;  /* don't let no-fix samples (numSats=0) zero it */
            s_st.hdop = atof(f[8]);
            if (nf > 9) s_st.alt = atof(f[9]);
        }
    } else if (!strcmp(type, "RMC")) {
        if (nf > 8) {
            if (f[1][0]) {
                s_st.has_time = true;
                s_st.utc_h = atoi(f[1]); s_st.utc_m = atoi(f[1] + 2); s_st.utc_s = atoi(f[1] + 4);
            }
            if (f[3][0]) { s_st.lat = nmea_deg(f[3], f[4]); s_st.has_pos = true; }
            if (f[5][0]) s_st.lon = nmea_deg(f[5], f[6]);
            s_st.speed_kmh = atof(f[7]) * 1.852f;
            if (nf > 9 && f[9][0]) {
                s_st.day = atoi(f[9]); s_st.month = atoi(f[9] + 2); s_st.year = 2000 + atoi(f[9] + 4);
            }
        }
    } else if (!strcmp(type, "GSA")) {
        snprintf(s_st.last_gsa, sizeof(s_st.last_gsa), "%s", raw);
        if (nf > 2) {
            int ft = atoi(f[2]);
            s_st.fix_type = (ft == 3) ? 2 : (ft == 2) ? 1 : 0;
            for (int i = 0; i < s_st.nsat; i++) s_st.sats[i].used = false;
            for (int i = 3; i < 15 && i < nf; i++) {
                if (f[i][0]) { gps_sat_t *s = sat_find_prn(atoi(f[i])); if (s) s->used = true; }
            }
        }
    } else if (!strcmp(type, "GSV")) {
        char sys = id[1]; /* G P/ B/ A/ L/ Q */
        for (int i = 4; i + 3 < nf; i += 4) {
            if (!f[i][0]) continue;
            gps_sat_t *s = sat_find_sys(sys, atoi(f[i]));
            if (!s) continue;
            s->seen = true;
            s->el = atoi(f[i + 1]);
            s->az = atoi(f[i + 2]);
            s->snr = f[i + 3][0] ? atoi(f[i + 3]) : 0;
        }
    } else if (!strcmp(type, "VTG")) {
        if (nf > 7 && f[7][0]) s_st.speed_kmh = atof(f[7]);
    } else if (!strcmp(type, "GLL")) {
        s_st.gll_count++;
    } else if (!strcmp(type, "TXT")) {
        snprintf(s_st.last_txt, sizeof(s_st.last_txt), "%s", raw);
        if (strstr(raw, "too strong")) s_st.txt_toostrong = true;
    }
}

/* ---- UBX binary framing (mixed with NMEA on the same stream) ---- */
static uint8_t s_ubx[200];
static int s_ubx_n = 0, s_ubx_need = 0, s_ubx_state = 0; /* 0 idle 1 B5 2 hdr 3 body */

static char gnss_sys_char(int gnssId) {
    switch (gnssId) {
    case 0: return 'P'; case 1: return 'S'; case 2: return 'A'; case 3: return 'B';
    case 4: return 'I'; case 5: return 'Q'; case 6: return 'L'; default: return '?';
    }
}

static void ubx_navsat(const uint8_t *p, int len) {
    if (len < 8) return;
    int n = p[5];
    for (int i = 0; i < n; i++) {
        int off = 8 + i * 12;
        if (off + 12 > len) break;
        const uint8_t *s = p + off;
        char sys = gnss_sys_char(s[0]);
        gps_sat_t *st = sat_find_sys(sys, s[1]);
        if (!st) continue;
        uint32_t fl = (uint32_t)s[8] | ((uint32_t)s[9] << 8) | ((uint32_t)s[10] << 16) | ((uint32_t)s[11] << 24);
        st->seen = true; st->ubx = true;
        st->snr = s[2];
        st->el = (int8_t)s[3];
        st->az = (int16_t)(s[4] | (s[5] << 8));
        st->used = (fl & 1) != 0;
        st->eph = (fl & 0x100) != 0;
        st->orbit = (fl >> 4) & 0x07;
    }
    s_st.have_navsat = true;
}

static void ubx_monhw(const uint8_t *p, int len) {
    if (len < 46) return;
    s_st.ant_status = p[20];
    s_st.ant_power = p[21];
    s_st.jam_state = (p[22] >> 2) & 0x03;
    s_st.jam_ind = p[45];
    s_st.have_monhw = true;
}

static void ubx_process(const uint8_t *f, int n) {
    if (n < 8) return;
    uint8_t cls = f[2], id = f[3];
    int len = f[4] | (f[5] << 8);
    if (6 + len + 2 > n) return;
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6 + len; i++) { a += f[i]; b += a; }
    if (a != f[6 + len] || b != f[7 + len]) return;
    const uint8_t *p = f + 6;
    if (cls == 0x0A && id == 0x04) {
        int m = len < 30 ? len : 30;
        memcpy(s_st.ver, p, m); s_st.ver[m] = 0; s_st.have_ver = true;
    } else if (cls == 0x01 && id == 0x35) {
        ubx_navsat(p, len);
    } else if (cls == 0x0A && id == 0x09) {
        ubx_monhw(p, len);
    }
}

static void feed_ubx_byte(uint8_t c) {
    switch (s_ubx_state) {
    case 0: if (c == 0xB5) { s_ubx[0] = c; s_ubx_n = 1; s_ubx_state = 1; } break;
    case 1: if (c == 0x62) { s_ubx[s_ubx_n++] = c; s_ubx_state = 2; } else s_ubx_state = 0; break;
    case 2: s_ubx[s_ubx_n++] = c;
        if (s_ubx_n == 6) {
            int len = s_ubx[4] | (s_ubx[5] << 8);
            s_ubx_need = 6 + len + 2;
            s_ubx_state = (s_ubx_need <= (int)sizeof(s_ubx)) ? 3 : 0;
        }
        break;
    case 3: s_ubx[s_ubx_n++] = c;
        if (s_ubx_n >= s_ubx_need) { ubx_process(s_ubx, s_ubx_n); s_ubx_state = 0; }
        break;
    }
}

static void feed(const uint8_t *buf, int r) {
    s_st.bytes_rx += r;
    s_st.last_rx_us = (uint64_t)esp_timer_get_time();
    for (int i = 0; i < r; i++) {
        uint8_t c = buf[i];
        if (s_ubx_state != 0) { feed_ubx_byte(c); continue; }
        if (c == 0xB5) { s_ln = 0; feed_ubx_byte(c); continue; }
        if (c == '\n' || c == '\r') {
            if (s_ln > 0) { s_line[s_ln] = 0; handle_line(s_line); s_ln = 0; }
        } else if (s_ln < (int)sizeof(s_line) - 1) {
            s_line[s_ln++] = (char)c;
        } else {
            s_ln = 0;
        }
    }
}

/* ================= transports ================= */
static void apply_baud(int baud) {
    uart_set_baudrate(GPS_UART, baud);
    uart_flush_input(GPS_UART);
    s_st.baud = baud;
}

void gps_begin_uart_pins(int rx, int tx) {
    if (!s_st.inited) { memset(&s_st, 0, sizeof(s_st)); s_st.fix_type = -1; }
    if (!s_uart_ready) {
        uart_driver_install(GPS_UART, 4096, 0, 0, NULL, 0);
        s_uart_ready = true;
    }
    uart_config_t cfg = {
        .baud_rate = BAUDS[s_baud_idx],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(GPS_UART, &cfg);
    uart_set_pin(GPS_UART, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    s_st.inited = true;
    s_st.source = 0;
    reset_counters();
    apply_baud(BAUDS[s_baud_idx]);
    ESP_LOGI(TAG, "GPS UART ESP_RX=%d ESP_TX=%d @ %d", rx, tx, BAUDS[s_baud_idx]);
    ubx_send(0x0A, 0x04, NULL, 0);   /* MON-VER poll to confirm TX works */
}

void gps_begin(void) {
    /* board labels are from the ESP view: GPS-tx=ESP TX(11), GPS-rx=ESP RX(12) */
    gps_begin_uart_pins(PIN_GPS_RX, PIN_GPS_TX);   /* ESP RX=12, ESP TX=11 */
}

void gps_scan_i2c(void) {
    if (!s_bus) return;
    s_st.i2c_found = 0;
    for (int a = 0x08; a < 0x78 && s_st.i2c_found < (int)sizeof(s_st.i2c_addrs); a++) {
        if (i2c_master_probe(s_bus, a, 30) == ESP_OK)
            s_st.i2c_addrs[s_st.i2c_found++] = (uint8_t)a;
    }
    ESP_LOGI(TAG, "I2C scan: %d device(s)", s_st.i2c_found);
    for (int i = 0; i < s_st.i2c_found; i++) ESP_LOGI(TAG, "  found 0x%02X", s_st.i2c_addrs[i]);
}

void gps_begin_i2c(void) {
    if (!s_st.inited) { memset(&s_st, 0, sizeof(s_st)); s_st.fix_type = -1; s_st.inited = true; }
    if (!s_bus) {
        i2c_master_bus_config_t bc = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = PIN_I2C_SDA,
            .scl_io_num = PIN_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
        if (e != ESP_OK) { ESP_LOGE(TAG, "i2c bus init: %s", esp_err_to_name(e)); return; }
    }
    if (!s_idev) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = NEO_ADDR,
            .scl_speed_hz = 100000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_idev) != ESP_OK) {
            ESP_LOGE(TAG, "i2c add 0x42 failed"); s_idev = NULL;
        }
    }
    s_st.inited = true;
    s_st.source = 1;
    reset_counters();
    gps_scan_i2c();
    /* poll UBX-MON-VER once to confirm the module answers UBX polls */
    ubx_send(0x0A, 0x04, NULL, 0);
    ESP_LOGI(TAG, "GPS I2C SDA=%d SCL=%d, %d dev found", PIN_I2C_SDA, PIN_I2C_SCL, s_st.i2c_found);
}

void gps_toggle_source(void) {
    if (s_st.source == 1) gps_begin();
    else gps_begin_i2c();
}

void gps_next_baud(void) {
    if (s_st.source == 1) { gps_scan_i2c(); return; }
    s_baud_idx = (s_baud_idx + 1) % NBAUD;
    apply_baud(BAUDS[s_baud_idx]);
    s_st.sentences = 0;
    s_st.crc_errors = 0;
    s_st.bytes_rx = 0;
    ESP_LOGI(TAG, "GPS baud -> %d", BAUDS[s_baud_idx]);
}

static const char *ant_str(int s) {
    switch (s) {
    case 0: return "INIT"; case 1: return "DONTKNOW"; case 2: return "OK";
    case 3: return "SHORT"; case 4: return "OPEN"; default: return "?";
    }
}

void gps_dump(void) {
    gps_state_t *g = &s_st;
    ESP_LOGI(TAG, "%s FIX %d  used %d  vis %d  ANT %s  pwr %s  JAM %d/%d  VER %.12s  GLL %lu",
             g->source == 1 ? "I2C" : "UART", g->fix_type, g->sats_used, g->sats_visible,
             g->have_monhw ? ant_str(g->ant_status) : "?",
             g->have_monhw ? (g->ant_power == 1 ? "ON" : (g->ant_power == 0 ? "OFF" : "?")) : "?",
             g->jam_ind, g->jam_state, g->have_ver ? g->ver : "-",
             (unsigned long)g->gll_count);

    /* BeiDou only.  format  B<prn>:<SNR>[*used][E=has ephemeris] */
    char line[150];
    int p = 0, n = 0;
    for (int i = 0; i < g->nsat; i++) {
        gps_sat_t *s = &g->sats[i];
        if (s->sys != 'B' || !s->seen || s->snr <= 0) continue;
        int room = (int)sizeof(line) - p;
        if (room < 16) break;                 /* leave room for NUL */
        int w = snprintf(line + p, (size_t)room, " B%d:%d%s%s",
                         s->prn, s->snr, s->used ? "*" : "", s->eph ? "E" : "");
        if (w < 0) break;
        if (w >= room) { p += room - 1; break; }  /* truncated: stop, stay in-bounds */
        p += w;
        n++;
    }
    line[p] = 0;
    ESP_LOGI(TAG, "BDS %d sv:%s", n, line);
    ESP_LOGI(TAG, "  GGA %s", g->last_gga);
    if (g->txt_toostrong) ESP_LOGW(TAG, "  !! RF INPUT TOO STRONG (front-end saturation) : %s", g->last_txt);
    else if (g->last_txt[0]) ESP_LOGI(TAG, "  TXT %s", g->last_txt);
}

/* ---- UBX send.
 * I2C (DDC) per u-blox 3.7.2.3: write the RAW UBX bytes to the 7-bit device
 * address (no register byte, >=2 bytes, STOP at the end). Reads use the
 * 0xFF(count)/0xFD(data) registers. UART: just write the UBX frame. ---- */
static bool ubx_send(uint8_t cls, uint8_t id, const uint8_t *payload, int n) {
    if (n > 56) return false;
    uint8_t buf[64];
    int k = 0;
    buf[k++] = 0xB5; buf[k++] = 0x62;
    buf[k++] = cls; buf[k++] = id;
    buf[k++] = (uint8_t)(n & 0xFF); buf[k++] = (uint8_t)(n >> 8);
    uint8_t a = 0, b = 0;
    for (int i = 0; i < k; i++) { a += buf[i]; b += a; }
    for (int i = 0; i < n; i++) { buf[k++] = payload[i]; a += payload[i]; b += a; }
    buf[k++] = a; buf[k++] = b;
    if (s_st.source == 1) {
        if (!s_idev) return false;
        return i2c_master_transmit(s_idev, buf, k, 200) == ESP_OK;
    }
    return uart_write_bytes(GPS_UART, buf, k) == k;
}

void gps_cfg_gps_bds(void) {
    if (!s_idev) return;
    /* 6 config blocks: gnssId,resTrkCh,maxTrkCh,reserved, flags(LE: bit0=enable) */
    uint8_t p[4 + 6 * 8];
    int k = 0;
    p[k++] = 0x00;               /* msgVer */
    p[k++] = 0x00;               /* numTrkChHw (ro) */
    p[k++] = 0xFF;               /* numTrkChUse = all */
    p[k++] = 0x06;               /* numConfigBlocks */
    struct { uint8_t id, res, max; uint32_t flags; } blk[6] = {
        {0, 8, 16, 0x00010001},  /* GPS   enable */
        {1, 1, 3,  0x00000000},  /* SBAS  off */
        {2, 4, 8,  0x00000000},  /* GAL   off */
        {3, 6, 12, 0x00010001},  /* BDS   enable */
        {5, 0, 3,  0x00000000},  /* QZSS  off */
        {6, 8, 14, 0x00000000},  /* GLO   off */
    };
    for (int i = 0; i < 6; i++) {
        p[k++] = blk[i].id; p[k++] = blk[i].res; p[k++] = blk[i].max; p[k++] = 0;
        p[k++] = (uint8_t)(blk[i].flags);
        p[k++] = (uint8_t)(blk[i].flags >> 8);
        p[k++] = (uint8_t)(blk[i].flags >> 16);
        p[k++] = (uint8_t)(blk[i].flags >> 24);
    }
    bool ok = ubx_send(0x06, 0x3E, p, k);
    ESP_LOGI(TAG, "UBX-CFG-GNSS (GPS+BDS only): %s", ok ? "sent" : "FAILED");
    /* controlled GNSS restart so the new constellation mask takes effect */
    uint8_t rst[4] = {0xFF, 0xFF, 0x02, 0x00};  /* navBbrMask=0xFFFF, mode=2 (GNSS start) */
    ubx_send(0x06, 0x04, rst, 4);
}

static int64_t s_last_poll_us = 0;

void gps_disable_gll(void) {
    /* CFG-MSG payload = msgClass, msgID, rate[6] (I2C, UART1, UART2, USB, SPI, res) */
    uint8_t p[8] = {0xF0, 0x01, 0, 0, 0, 0, 0, 0};   /* NMEA GLL, all ports rate 0 */
    bool ok = ubx_send(0x06, 0x01, p, 8);
    ESP_LOGI(TAG, "UBX-CFG-MSG disable GLL (8B): %s (gll_count=%lu)",
             ok ? "sent" : "FAILED", (unsigned long)s_st.gll_count);
}

void gps_restart(void) {
    /* navBbrMask=0xFFFF (clear all -> cold), resetMode=0x02 (controlled GNSS start) */
    uint8_t rst[4] = {0xFF, 0xFF, 0x02, 0x00};
    ubx_send(0x06, 0x04, rst, 4);
    ESP_LOGI(TAG, "GNSS restart (cold) sent on %s", s_st.source == 1 ? "I2C" : "UART");
}

void gps_poll(void) {
    if (!s_st.inited) return;
    int64_t now = esp_timer_get_time();
    if (now - s_last_poll_us > 2000000) {
        s_last_poll_us = now;
        ubx_send(0x01, 0x35, NULL, 0);   /* UBX-NAV-SAT poll */
        ubx_send(0x0A, 0x09, NULL, 0);   /* UBX-MON-HW poll */
    }
    if (s_st.source == 1) {
        if (!s_idev) return;
        for (int k = 0; k < 4; k++) {
            uint8_t reg = 0xFF, cnt[2];
            if (i2c_master_transmit_receive(s_idev, &reg, 1, cnt, 2, 30) != ESP_OK) break;
            int avail = ((cnt[0] << 8) | cnt[1]) & 0xFFFF;
            if (avail <= 0) break;
            if (avail > 128) avail = 128;
            uint8_t reg2 = 0xFD, buf[128];
            if (i2c_master_transmit_receive(s_idev, &reg2, 1, buf, avail, 50) != ESP_OK) break;
            feed(buf, avail);
        }
    } else {
        uint8_t buf[256];
        for (;;) {
            int r = uart_read_bytes(GPS_UART, buf, sizeof(buf), 0);
            if (r <= 0) break;
            feed(buf, r);
        }
    }
    int vis = 0;
    for (int i = 0; i < s_st.nsat; i++) if (s_st.sats[i].seen && s_st.sats[i].snr > 0) vis++;
    s_st.sats_visible = vis;
}
