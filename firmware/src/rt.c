#include "rt.h"
#include <math.h>

static rt_path_t s_rt;

rt_path_t *rt_get(void) { return &s_rt; }

void rt_reset(rt_path_t *p) {
    p->has_ref = false;
    p->n = 0;
    p->dist = 0;
    p->min_move = 1.5f;
}

bool rt_add(rt_path_t *p, double lat, double lon) {
    if (!p->has_ref) {
        p->ref.lat0 = lat;
        p->ref.lon0 = lon;
        p->has_ref = true;
        p->n = 0;
        p->dist = 0;
    }
    float x, y;
    geo_meters(&p->ref, lat, lon, &x, &y);
    pt_t q = {x, y};
    if (p->n == 0) {
        p->pts[0] = q;
        p->n = 1;
        return true;
    }
    pt_t last = p->pts[p->n - 1];
    float d = hypotf(q.x - last.x, q.y - last.y);
    if (d < p->min_move) return false;
    p->dist += d;
    if (p->n < RT_MAX) p->pts[p->n++] = q;
    return true;
}
