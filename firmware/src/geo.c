#include "geo.h"
#include <math.h>

#define GEO_R 6378137.0

void geo_meters(const geo_ref_t *r, double lat, double lon, float *mx, float *my) {
    double dlat = (lat - r->lat0) * (M_PI / 180.0);
    double dlon = (lon - r->lon0) * (M_PI / 180.0);
    double mlat = (lat + r->lat0) * 0.5 * (M_PI / 180.0);
    *mx = (float)(dlon * cos(mlat) * GEO_R);
    *my = (float)(dlat * GEO_R);
}
