#pragma once
// Pure device-identity helpers, split out from main.cpp so they can be unit
// tested on the host (see firmware/host_test/test_deviceid.cpp). No Arduino deps.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

// The device name becomes an MQTT topic segment (freezer/<name>/...), the
// InfluxDB `device` tag, the MQTT client id, and the AP SSID suffix
// "freezermon-<name>". Two hard constraints:
//   - charset must be topic/tag-safe: no '/', '+', '#', control chars, spaces.
//   - length: the SSID must stay within the 32-char 802.11 limit, so
//     max name = 32 - strlen("freezermon-") = 32 - 11 = 21. A longer name
//     makes WiFi.softAP() reject the SSID and the debug AP never starts,
//     which would strand /setname (the only field rename path).
static const size_t DEVICE_NAME_MAX = 21;

inline bool deviceNameValid(const char *n) {
  size_t len = strlen(n);
  if (len < 1 || len > DEVICE_NAME_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    char c = n[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

// Unique-per-chip default name. ESP.getEfuseMac() packs the 6 MAC octets into
// the low 48 bits with octet 0 in the LSB, so the low 3 bytes are the shared
// Espressif OUI (identical across a production reel) and the TOP 3 bytes are
// the NIC-unique portion. Derive from the unique bytes so two un-named units
// never collide. Pure -> host-testable.
inline void chipSeedName(uint64_t efuseMac, char *out, size_t outLen) {
  uint32_t uniq = (uint32_t)((efuseMac >> 24) & 0xFFFFFF);
  snprintf(out, outLen, "cooler-%06x", uniq);
}
