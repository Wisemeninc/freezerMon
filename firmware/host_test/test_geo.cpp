// Host-side tests for include/geo.h (movement detection geometry).
//   g++ -std=c++17 -I firmware/include firmware/host_test/test_geo.cpp -o /tmp/tg && /tmp/tg
#include "geo.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool near(float a, float b, float tolPct) {
  return fabsf(a - b) <= b * tolPct / 100.0f;
}

int main() {
  // identical points -> 0
  assert(geoDistM(55.99f, 9.958f, 55.99f, 9.958f) == 0.0f);

  // 0.001 deg latitude ~= 110.6 m anywhere
  assert(near(geoDistM(55.990f, 9.958f, 55.991f, 9.958f), 110.6f, 2.0f));

  // 0.001 deg longitude at 56N ~= 111320*cos(56 deg)/1000 ~= 62.3 m
  assert(near(geoDistM(55.990f, 9.958f, 55.990f, 9.959f), 62.3f, 2.0f));

  // symmetric
  assert(geoDistM(55.0f, 9.0f, 55.01f, 9.01f) == geoDistM(55.01f, 9.01f, 55.0f, 9.0f));

  // movement-detection sanity at the configured thresholds:
  // GPS scatter observed on the bench (~55.9911 vs 55.9913, ~25 m) must stay
  // below MOVE_ALARM_M=150, and a genuine move of ~0.002 deg lat (~221 m) must trip it.
  float scatter = geoDistM(55.99113f, 9.95785f, 55.99131f, 9.95794f);
  assert(scatter < 150.0f);
  float moved = geoDistM(55.9911f, 9.9578f, 55.9931f, 9.9578f);
  assert(moved > 150.0f);

  printf("geo: all tests passed (scatter=%.0fm moved=%.0fm)\n", scatter, moved);
  return 0;
}
