/* WGS84 lat/lon -> local metric plane (equirectangular around a reference).
 * Good enough for track-sized distances (hundreds of metres). */
#pragma once

typedef struct { double lat0, lon0; } geo_ref_t;

void geo_meters(const geo_ref_t *r, double lat, double lon, float *mx, float *my); /* x=east, y=north, metres */
