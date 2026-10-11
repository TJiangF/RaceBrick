#include "util.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "esp_random.h"

void u_srand(uint32_t seed) { srand(seed); }
float u_randf(void) { return (float)rand() / ((float)RAND_MAX + 1.0f); }
float u_rand_pm(void) { return u_randf() * 2.0f - 1.0f; }

void u_fmt_f(char *out, int cap, float v, int dp) {
    if (cap <= 0) return;
    if (dp < 0) dp = 0;
    if (dp > 6) dp = 6;
    if (!isfinite((double)v)) { snprintf(out, cap, "0"); return; }
    int neg = v < 0;
    double a = neg ? -(double)v : (double)v;
    static const long mul[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    long long s = (long long)(a * mul[dp] + 0.5);
    long long ip = s / mul[dp];
    long long fp = s % mul[dp];
    char tmp[40];
    int n = 0;
    if (neg) tmp[n++] = '-';
    char rev[24];
    int rn = 0;
    if (ip == 0) rev[rn++] = '0';
    while (ip) { rev[rn++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    while (rn) tmp[n++] = rev[--rn];
    if (dp) {
        tmp[n++] = '.';
        for (int i = dp - 1; i >= 0; i--) tmp[n++] = (char)('0' + (int)((fp / mul[i]) % 10));
    }
    tmp[n] = 0;
    snprintf(out, cap, "%s", tmp);
}

void u_fmt_time(char *out, int cap, float t) {
    if (!isfinite((double)t)) { snprintf(out, cap, "--:--.---"); return; }
    int neg = t < 0;
    double a = neg ? -(double)t : (double)t;
    long long total = (long long)(a * 1000.0 + 0.5);
    int minutes = (int)(total / 60000);
    int rem = (int)(total % 60000);
    int sec = rem / 1000;
    int ms = rem % 1000;
    snprintf(out, cap, "%s%02d:%02d.%03d", neg ? "-" : "", minutes, sec, ms);
}

void u_fmt_delta(char *out, int cap, float d, int digits) {
    int neg = d < 0;
    double a = neg ? -(double)d : (double)d;
    char b[24];
    u_fmt_f(b, sizeof b, (float)a, digits);
    snprintf(out, cap, "%s%s", neg ? "-" : "+", b);
}
