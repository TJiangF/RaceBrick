/* Small helpers: clamping, random, and float formatting without printf("%f").
 * Float formatting is hand-rolled so it works regardless of newlib nano config.
 */
#pragma once
#include <stdint.h>

static inline float u_clampf(float v, float a, float b) {
    return v < a ? a : (v > b ? b : v);
}
static inline int u_clampi(int v, int a, int b) {
    return v < a ? a : (v > b ? b : v);
}
static inline float u_absf(float x) { return x < 0 ? -x : x; }

void  u_srand(uint32_t seed);
float u_randf(void);          /* [0,1)   */
float u_rand_pm(void);        /* [-1,1)  */

/* fixed point float -> string. `dp` 0..3. Always writes sign when signed!=0. */
void  u_fmt_f(char *out, int cap, float v, int dp);

/* seconds -> "MM:SS.mmm" (or "--:--.---" for non-finite), matching dom.js */
void  u_fmt_time(char *out, int cap, float t);
/* signed seconds -> "+D.ddd" */
void  u_fmt_delta(char *out, int cap, float d, int digits);
