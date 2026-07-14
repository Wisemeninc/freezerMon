// Host-side unit tests for the pure device-identity helpers in include/deviceid.h.
// Deliberately NOT under firmware/test/ so PlatformIO never tries to cross-compile
// it onto the device. Build + run on any host with g++:
//   g++ -std=c++17 -I firmware/include firmware/host_test/test_deviceid.cpp -o /tmp/t && /tmp/t
#include "deviceid.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
  // --- deviceNameValid: charset must block MQTT topic specials + unsafe chars ---
  assert(deviceNameValid("cooler-01"));
  assert(deviceNameValid("a"));
  assert(!deviceNameValid(""));            // empty
  assert(!deviceNameValid("cooler/01"));   // topic separator
  assert(!deviceNameValid("cooler+01"));   // single-level wildcard
  assert(!deviceNameValid("cooler#01"));   // multi-level wildcard
  assert(!deviceNameValid("Cooler-01"));   // uppercase
  assert(!deviceNameValid("cooler 01"));   // space
  assert(!deviceNameValid("cooler.01"));   // dot
  assert(!deviceNameValid("cooler_01"));   // underscore

  // --- length cap = 21 so "freezermon-<name>" stays within the 32-char SSID limit (#2) ---
  char n21[64]; memset(n21, 'a', 21); n21[21] = 0;
  assert(deviceNameValid(n21));            // 21 ok  -> SSID = 11 + 21 = 32
  char n22[64]; memset(n22, 'a', 22); n22[22] = 0;
  assert(!deviceNameValid(n22));           // 22 rejected -> guards the softAP AP-lockout

  // --- chipSeedName derives from the NIC-unique MAC octets, not the shared OUI (#1) ---
  // two chips: SAME low-3 (OUI) bytes, DIFFERENT top-3 (NIC) bytes.
  uint64_t chipA = ((uint64_t)0x111111 << 24) | 0x240AC4;  // NIC 111111, OUI 240ac4
  uint64_t chipB = ((uint64_t)0x222222 << 24) | 0x240AC4;  // NIC 222222, same OUI
  char sa[32], sb[32];
  chipSeedName(chipA, sa, sizeof(sa));
  chipSeedName(chipB, sb, sizeof(sb));
  assert(strcmp(sa, sb) != 0);             // would COLLIDE under the old (mac & 0xFFFFFF) bug
  assert(strcmp(sa, "cooler-111111") == 0);// maps the unique top-3 octets
  assert(deviceNameValid(sa));             // every derived default is a valid name

  printf("deviceid: all tests passed\n");
  return 0;
}
