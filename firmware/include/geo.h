#pragma once
// Pure geo helper, host-testable (see firmware/host_test/test_geo.cpp).
#include <math.h>

// Distance in meters between two WGS84 points, equirectangular approximation —
// error <1% for the few-km displacements movement detection cares about, and
// far cheaper than haversine on the ESP32 FPU.
inline float geoDistM(float lat1, float lon1, float lat2, float lon2) {
  float mlat = (lat1 + lat2) * 0.5f * (float)M_PI / 180.0f;
  float dx = (lon2 - lon1) * 111320.0f * cosf(mlat);   // meters per deg lon at this latitude
  float dy = (lat2 - lat1) * 110574.0f;                // meters per deg lat
  return sqrtf(dx * dx + dy * dy);
}
