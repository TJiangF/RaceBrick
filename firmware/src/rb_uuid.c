#include "rb_uuid.h"

#include <stdio.h>
#include <string.h>
#include "esp_random.h"

void rb_uuid_clear(rb_uuid_t *u) {
    if (u) memset(u, 0, sizeof(*u));
}

bool rb_uuid_is_zero(const rb_uuid_t *u) {
    if (!u) return true;
    for (int i = 0; i < 16; i++) if (u->b[i]) return false;
    return true;
}

bool rb_uuid_equal(const rb_uuid_t *a, const rb_uuid_t *b) {
    if (!a || !b) return false;
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

void rb_uuid_new(rb_uuid_t *u) {
    if (!u) return;
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(&u->b[i], &r, 4);
    }
    u->b[6] = (uint8_t)((u->b[6] & 0x0F) | 0x40);
    u->b[8] = (uint8_t)((u->b[8] & 0x3F) | 0x80);
}

void rb_uuid_format(const rb_uuid_t *u, char *out, int cap) {
    if (!out || cap <= 0) return;
    if (!u) {
        snprintf(out, cap, "00000000-0000-0000-0000-000000000000");
        return;
    }
    snprintf(out, cap,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u->b[0], u->b[1], u->b[2], u->b[3], u->b[4], u->b[5],
             u->b[6], u->b[7], u->b[8], u->b[9], u->b[10], u->b[11],
             u->b[12], u->b[13], u->b[14], u->b[15]);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool rb_uuid_parse(const char *text, rb_uuid_t *u) {
    if (!text || !u) return false;
    int n = 0;
    for (const char *p = text; *p && n < 16; ) {
        if (*p == '-') { p++; continue; }
        int hi = hex_digit(p[0]);
        int lo = hex_digit(p[1]);
        if (hi < 0 || lo < 0) return false;
        u->b[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    return n == 16;
}
