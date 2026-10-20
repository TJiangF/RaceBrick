/* Real-time GPS path capture (metres relative to the first fix). */
#pragma once
#include <stdbool.h>
#include "sim.h"   /* pt_t */
#include "geo.h"

#define RT_MAX 800

typedef struct {
    geo_ref_t ref;
    bool  has_ref;
    pt_t  pts[RT_MAX];
    int   n;
    float dist;        /* path length, metres */
    float min_move;    /* only store a point after this much movement */
} rt_path_t;

rt_path_t *rt_get(void);
void rt_reset(rt_path_t *p);
bool rt_add(rt_path_t *p, double lat, double lon);   /* true if a point was appended */
