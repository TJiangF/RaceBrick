/* Runtime palette (dark / daylight) in RGB565 — mirrors RC.THEMES. */
#pragma once
#include <stdint.h>

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint16_t bg, stage, panel, line, text, dim, white;
    uint16_t cyan, green, red, ledoff, purple, gold, yellow, orange;
    uint16_t devicebg, statusbg, surface, surface2, line2, seltint, livebg, text2;
    uint16_t glyphoff, dotoff, checkboxoff, goldtint, bestborder, prn;
    uint16_t mapbg, mapgrid, mapborder, centerline, histtrace, curtrace;
    uint16_t gpstrace, gpstracehist, gmring, gmtrail, gmcenter;
} palette_t;

extern palette_t PAL;

void theme_set(int light); /* 0 = dark, 1 = light */
int  theme_is_light(void);
