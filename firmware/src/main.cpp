/**
 * freezerMon — mobile cooling unit monitor
 * Board: LilyGO T-A7608E-H (ESP32-WROVER-E + SIMCom A7608E-H, LTE Cat-4)
 *
 * Battery regime: wake (timer or door interrupt) -> read sensors -> evaluate
 *   alarm -> power modem, attach LTE, publish (+ flush offline buffer) ->
 *   modem off -> deep sleep. Sleep is the guaranteed terminal state: a task
 *   watchdog resets the chip if the network path wedges past the awake budget.
 *
 * Powered regime (external power present): deep sleep is a battery measure,
 *   so the device stays awake with the LTE/MQTT session up, polls the door
 *   every 250 ms (instant alerts) and reports every REPORT_INTERVAL_POWERED_S.
 *   Falls back to the battery regime when power is pulled or the link drops.
 */
#include "config.h"

#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <rom/rtc.h>             // rtc_get_reset_reason(core) — ROM-level reset forensics
#include <time.h>
#include <sys/time.h>          // gettimeofday: the RTC clock keeps running in deep sleep -> measured sleep length
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>         // NVS-backed device name (survives OTA; set via /setname)
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>    // modemLock: the dbgweb task and the main thread share one modem UART
#include "mbedtls/md.h"          // streaming SHA-256 (version-stable md_* API)
#include "mbedtls/pk.h"          // RSA-2048 signature verify
#include "ota_pubkey.h"          // OTA_PUBKEY_PEM — image-signing public key
#include "deviceid.h"            // deviceNameValid(), chipSeedName() — host-testable
#include "geo.h"                 // geoDistM() — movement detection, host-testable

#define FW_VERSION "2.91"   // verNewer() compares dotted INTEGER components: 2.10 > 2.9, and 2.7 < 2.68 — never drop a trailing digit

#define SerialMon Serial
#define SerialAT  Serial1

TinyGsm modem(SerialAT);
#ifdef USE_TLS
TinyGsmClientSecure netClient(modem);   // MQTT over TLS on mux 0
#else
TinyGsmClient netClient(modem);
#endif
// OTA/manifest fetches run over PLAIN HTTP on a second session (mux 1). The
// A76XX's second-TLS-session handshake was unreliable and cost a whole day of
// failed self-updates; a plain-TCP fetch (port 80 -> CCHOPEN client_type=1, no
// TLS handshake) sidesteps it. Access control is the unguessable token path;
// Update.begin/end validate the image, so a corrupt/truncated download fails
// safely. Non-secret payload, so no confidentiality need on the wire.
TinyGsmClient otaHttpClient(modem, 1);
PubSubClient mqtt(netClient);

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature probes(&oneWire);

// ---------- state that survives deep sleep ----------
struct Sample {
  uint32_t ageS;      // seconds since taken (incremented across sleeps)
  float    tCab;
  float    tAmb;
  uint16_t vbatMv;
  uint16_t vsolarMv;
  uint8_t  doorOpen;
  uint8_t  alarm;
  uint8_t  extPower;   // 1 = running on external power, 0 = on battery
};
#define BUF_MAX 24
RTC_DATA_ATTR Sample   rtcBuf[BUF_MAX];
RTC_DATA_ATTR uint8_t  rtcBufCount = 0;
RTC_DATA_ATTR uint8_t  consecutiveBreaches = 0;
RTC_DATA_ATTR uint8_t  alarmActive = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint8_t  reportsSinceGps = 0xFF;  // force fix on first boot
RTC_DATA_ATTR uint8_t  gnssZeroSatStreak = 0;   // GNSS-wedge watchdog: consecutive attempts tracking 0 sats
RTC_DATA_ATTR float    lastLat = 0, lastLon = 0; // most recent fix — movement baseline; NOT auto-published (see gpsFreshThisWake)
static bool gpsFreshThisWake = false;            // set only by a successful fix THIS wake — a published coordinate is a measurement, not a memory
static float lastFixHdop = 0;                    // quality of the fix behind lastLat/lastLon (published with it)
RTC_DATA_ATTR int64_t sleepStartUs = 0;          // RTC clock at deep-sleep entry; measured sleep length is applied at the next boot
RTC_DATA_ATTR uint8_t gpsMissStreak = 0;         // consecutive attempts with no gate-passing fix (indoors) — drives the backoff
// Outcome of the most recent GNSS attempt, published in EVERY frame as gps_last /
// gps_last_s / gps_last_hdop so a failed hunt is visible from the dashboard rather
// than indistinguishable from "did not try" (the frame goes out before the hunt,
// and only a success sends the follow-up). RTC: survives sleep, not brownouts.
//   0 = no attempt yet   1 = no fix in window   2 = fix seen but rejected by the gate   3 = fix published
//   4 = good fix obtained but the follow-up frame could not be sent (MQTT down after GNSS, even after a reconnect)
RTC_DATA_ATTR uint8_t  gpsLastStatus = 0;
RTC_DATA_ATTR uint16_t gpsLastSecs   = 0;
RTC_DATA_ATTR float    gpsLastHdop   = 0;         // best HDOP seen in that window (rejected or not)
RTC_DATA_ATTR uint8_t  gpsLastSats   = 0;         // most satellites in view at any poll of that window (fix or not)
RTC_DATA_ATTR char     gpsLastSvs[16] = "";       // per-constellation split at that moment, in sentence order (3 fields on this firmware: GPS/GLONASS/BeiDou)
RTC_DATA_ATTR char     gnssModeStr[12] = "";      // +CGNSSMODE? read-back after requesting all constellations
RTC_DATA_ATTR char     gnssModesStr[40] = "";     // +CGNSSMODE=? — the modes THIS modem firmware accepts
#ifndef GPS_MISS_BACKOFF_AFTER
#define GPS_MISS_BACKOFF_AFTER 3   // after this many straight misses, hunt only every GPS_MISS_BACKOFF_N wakes
#endif
#ifndef GPS_MISS_BACKOFF_N
#define GPS_MISS_BACKOFF_N     3   // (a success snaps back to GPS_EVERY_N_REPORTS)
#endif
static int   lastFixSats = 0;
#ifndef GNSS_MODE_STR
// +CGNSSMODE on THIS A7608E-H firmware accepts (1-3) (read back 2026-08-26 20:47Z; 15 was refused):
//   1 = GPS+GLONASS+Galileo+SBAS+QZSS (the default it was running)   2 = BeiDou only   3 = all of them
// The 3-field +CGNSSINFO count is GPS / GLONASS / BeiDou; Galileo is folded in, not reported separately.
#define GNSS_MODE_STR "3"
#endif
#ifndef GPS_ZERO_SAT_ABORT_S
#define GPS_ZERO_SAT_ABORT_S 30   // a healthy engine sees satellites within seconds; 0 in view after this long = wedged engine or no antenna -> stop wasting the window
#endif
#ifndef GPS_MAX_HDOP
#define GPS_MAX_HDOP   2.5f   // reject fixes with worse horizontal dilution (typical good fix: 0.8-1.5)
#endif
#ifndef GPS_REQUIRE_3D
#define GPS_REQUIRE_3D 1      // 2D fixes (no altitude solution) are also horizontally poor — reject
#endif
#ifndef GPS_SETTLE_S
#define GPS_SETTLE_S   10     // keep polling this long after the first good fix, publish the lowest-HDOP sample
#endif
static bool agpsLoaded = false;                  // ephemeris lives in GNSS-engine RAM: false after every modem power cycle
RTC_DATA_ATTR uint32_t rtcEpoch = 0;            // best-known epoch, aged across sleeps
RTC_DATA_ATTR uint32_t lastAgpsEpoch = 0;       // when AGPS ephemeris was last downloaded (validity ~hours)
// movement detection: position is anchored while parked; a fix > MOVE_ALARM_M
// from the anchor flags the unit as moving (alert + fast cadence + GPS every
// wake) until MOVE_STOP_CYCLES near-still fixes re-anchor it.
RTC_DATA_ATTR float    anchorLat = 0, anchorLon = 0;
RTC_DATA_ATTR uint8_t  movingActive = 0;        // in telemetry as `moving`; drives fast cadence
RTC_DATA_ATTR uint8_t  stillStreak = 0;         // consecutive near-still fixes while moving
RTC_DATA_ATTR uint8_t  moveAlertPending = 0;    // `moving` alert queued until MQTT is next up
RTC_DATA_ATTR uint8_t  tempAlertPending = 0;    // `temp_breach` alert queued — the latch can happen on a boot that dies before MQTT (double-boot pattern), so the edge is decoupled from the publish
static Sample lastSample; static bool lastSampleValid = false;   // main thread's most recent reading, for /status
RTC_DATA_ATTR uint8_t  coldChargeActive = 0;    // cold-charge condition latched (one-shot alert + re-arm hysteresis)

uint32_t awakeStart = 0;

// powered-mode session state (external power -> no deep sleep needed)
bool     poweredSession = false;
uint8_t  lastDoor = 0;
uint8_t  prevAlarm = 0;
uint32_t lastReportMs = 0;

// ---------- LTE OTA state ----------
// A retained JSON on freezer/<id>/cmd declares the desired firmware:
//   {"ota_url":"https://host/fw/<token>/firmware.bin","ota_ver":"1.4"}
// The device acts only when ota_ver differs from FW_VERSION and power allows.
char          otaUrl[192] = "";
char          otaVer[16]  = "";
char          otaMd5[33]  = "";    // hex MD5 of the whole image; Update verifies it so a
                                   // corrupt assembly is never installed (empty = skip check)
volatile long otaSize     = 0;     // total image size; pieces are otaUrl.000, .001, …
volatile long otaSkip     = 4096;  // bytes to skip at the end of each piece (the modem
                                   // drops the last ~3072 of every cached response);
                                   // pieces overlap by this much so no data is lost.
volatile bool otaPending  = false;
volatile bool otaCheckRequested = false;   // /update "check online" — set by dbgweb task, consumed by main thread

static void logLine(const char *fmt, ...);   // defined below (ring-buffer logger)

// Strictly-newer version compare ("2.48" > "2.47"). Anti-rollback: OTA fires only
// for a newer version, so an attacker who can inject a command can't force a
// downgrade to an older (validly signed but vulnerable) build. Deliberate
// downgrades are done over UART.
static bool verNewer(const char *cand, const char *cur) {
  int c1 = 0, c2 = 0, r1 = 0, r2 = 0;
  if (sscanf(cand, "%d.%d", &c1, &c2) < 1) return false;
  sscanf(cur, "%d.%d", &r1, &r2);
  return (c1 != r1) ? (c1 > r1) : (c2 > r2);
}

// Reject an OTA URL that could break out of the AT+HTTPPARA="URL","..." line and
// inject AT commands (CR/LF or a stray quote) or overflow otaUrl[192]/purl[224].
// ArduinoJson decodes \r\n escapes into real control bytes, so this must run on the
// decoded string. Allow only a conservative printable URL charset.
static bool otaUrlSafe(const char *u) {
  size_t n = u ? strlen(u) : 0;
  if (n < 8 || n > 180) return false;
  if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)u[i];
    if (c < 0x21 || c > 0x7e) return false;            // no controls/space/non-ASCII (blocks CR/LF)
    if (c == '"' || c == '\\') return false;           // no quote/backslash
  }
#ifdef OTA_MANIFEST_URL
  // Host pin: the device only ever legitimately fetches from the manifest's host. Without
  // this a cmd publisher could drive the fleet to GET any host:port reachable from the
  // cellular bearer (carrier-internal RFC1918 included) with /log as a status oracle.
  {
    const char *m = OTA_MANIFEST_URL; m += strncmp(m, "https://", 8) == 0 ? 8 : 7;
    const char *a = u + (strncmp(u, "https://", 8) == 0 ? 8 : 7);
    size_t ml = strcspn(m, "/:"), al = strcspn(a, "/:");
    if (ml == 0 || ml != al || strncasecmp(m, a, ml) != 0) return false;
  }
#endif
  return true;
}

static bool verStrictOk(const char *v) {           // ^[0-9]{1,3}\.[0-9]{1,3}$
  int a = 0, b = 0, dots = 0;
  for (const char *c = v; *c; c++) {
    if (*c == '.') { if (++dots > 1) return false; }
    else if (*c < '0' || *c > '9') return false;
    else if (dots == 0) { if (++a > 3) return false; } else { if (++b > 3) return false; }
  }
  return dots == 1 && a > 0 && b > 0;
}
static volatile bool mqttInboundSeen = false;   // any downlink this session
static volatile bool mqttEchoSeen = false;      // the broker returned OUR retained telemetry frame: proof the publish path delivered
static bool pendingFixSent = false;             // publishPendingFix wrote a backfill frame this wake
static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  mqttInboundSeen = true;
  // Our own live telemetry frame is published retained; subscribing to that topic makes
  // the broker hand it straight back. Receiving it proves the broker STORED what this
  // session published — the only delivery evidence QoS-0 PubSubClient can give us
  // (publish() returns true into dead sockets). A cmd downlink is optional and may not
  // exist (review 2026-08-27), so it is no longer the liveness signal.
  if (strstr(topic, "/telemetry")) { mqttEchoSeen = true; return; }
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char *u = doc["ota_url"], *v = doc["ota_ver"];
  // ota_ver is echoed into log lines (and from there into crash_log telemetry): accept
  // only <1-3 digits>.<1-3 digits>, the same contract PublishFirmware.ts enforces.
  if (v && !verStrictOk(v)) { logLine("[ota] rejected malformed ota_ver"); return; }
  if (u && v && verNewer(v, FW_VERSION)) {           // anti-rollback: newer only
    if (!otaUrlSafe(u)) { logLine("[ota] rejected unsafe url"); return; }
    strlcpy(otaUrl, u, sizeof(otaUrl));
    strlcpy(otaVer, v, sizeof(otaVer));
    otaSize = doc["ota_size"] | 0L;
    otaSkip = doc["ota_skip"] | 4096L;    // server sets = piece overlap; default keeps old cmds working
    const char *m = doc["ota_md5"];
    strlcpy(otaMd5, m ? m : "", sizeof(otaMd5));
    otaPending = true;
  }
}

// ---------- field-debug console state ----------
WebServer debugServer(80);
bool debugApActive = false;
volatile bool modemBusy = false;   // main thread owns the modem UART while set — written ONLY by the
                                   // main thread; /update's flash-claim reads it under flashMux
// The dbgweb task (/lte /gps /sms) and the main thread share one modem UART. modemBusy
// is a flag, not a lock: a console handler that raced past it and then cleared it
// released the main thread's claim mid-OTA (2026-08-26 review). This recursive mutex
// is the owner token: the main thread holds it for a whole modem session (and nests
// it inside performOta/checkOnlineUpdate); console handlers try-take with zero wait
// and answer "busy". Never assigned from a task that did not acquire it.
static SemaphoreHandle_t modemLock = nullptr;
static bool consoleTakeModem() { return modemLock && xSemaphoreTakeRecursive(modemLock, 0) == pdTRUE; }
static void consoleGiveModem() { if (modemLock) xSemaphoreGiveRecursive(modemLock); }
static void mainTakeModem()    { if (modemLock) xSemaphoreTakeRecursive(modemLock, portMAX_DELAY); }
static void mainGiveModem()    { if (modemLock) xSemaphoreGiveRecursive(modemLock); }
// Scope guard for console handlers: the give happens on every exit path, so a future
// early `return` inside a handler cannot leave the console locked until reboot.
struct ConsoleModemHold { bool held; ConsoleModemHold() : held(consoleTakeModem()) {} ~ConsoleModemHold() { if (held) consoleGiveModem(); } };

// Same-origin gate for every state-changing console endpoint (/setname, /update,
// /update/check). Fail-closed: a request with NO Origin header is refused as well —
// browsers always send it on cross-origin POSTs, so a missing header is a client we
// cannot vouch for. Stops a drive-by page on the AP from erasing the inactive OTA
// slot or renaming the unit.
// The AP address is fixed by the ESP32 default softAP config (no softAPConfig call); if
// that ever becomes configurable, derive this from WiFi.softAPIP(). No port: the console
// listens on :80 only.
static bool sameOriginRequest() {
  if (debugServer.hasHeader("Origin") && debugServer.header("Origin") == "http://192.168.4.1") return true;
  // Scripted provisioning (curl) sends no Origin. A custom header cannot be attached to a
  // cross-origin browser request without a CORS preflight this server never answers, so
  // requiring it is exactly as strong as the Origin check for the CSRF case.
  return debugServer.hasHeader("X-FreezerMon");
}

// Runtime device identity. Resolved once per boot from NVS (survives OTA, since
// OTA only rewrites the app partition) so ONE firmware image can serve a whole
// fleet — each unit is named independently via the /setname console endpoint.
// DEVICE_ID in config.h is only the seed default for an un-provisioned unit.
char deviceId[32] = "cooler-01";   // safe placeholder until resolveDeviceId() runs

#define PAYLOAD_MAX 1152 // MQTT frame buffer; mqtt.setBufferSize must exceed it plus topic + headers
#define LOG_RING 96      // holds a full multi-chunk OTA trace in /log
#define LOG_LINE 160
// Fixed-size ring, no heap: logLine() runs on BOTH the main thread and the dbgweb
// task while /log reads it. A String ring reallocated under a concurrent copy is a
// use-after-free that panics the unit exactly when someone is reading the log
// (2026-08-26 independent review).
static char    logRing[LOG_RING][LOG_LINE];
static uint8_t logHead = 0, logCount = 0;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
// Crash forensics: the last few log lines live in RTC memory that is NOT zeroed by
// software / watchdog / panic resets (RTC_NOINIT_ATTR), so the boot after an
// INT_WDT / TASK_WDT / PANIC can publish what the firmware was doing when it died
// (crash_log field). Power-on and brownout resets DO clear RTC — this is a
// firmware-fault tool, not a supply-fault tool. Snapshotted at the top of setup()
// before this boot's own log lines overwrite it. Two tasks write it unlocked; a
// torn line in a diagnostic is acceptable, a spinlock in the log path is not.
#define CRASH_LINES 4
#define CRASH_LINE  72
RTC_NOINIT_ATTR char     crashRing[CRASH_LINES][CRASH_LINE];
RTC_NOINIT_ATTR uint8_t  crashHead;
RTC_NOINIT_ATTR uint32_t crashMagic;
static String crashSnapshot;                     // filled at boot after a WDT/PANIC reset, published once

// log to serial AND the ring buffer served at /log (no trailing newline in fmt)
static void logLine(const char *fmt, ...) {
  char buf[LOG_LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SerialMon.println(buf);
  if (crashMagic != 0xC0DEC0DEUL || crashHead >= CRASH_LINES) { memset(crashRing, 0, sizeof(crashRing)); crashHead = 0; crashMagic = 0xC0DEC0DEUL; }
  // The ring is published as telemetry (crash_log) over a session the modem does not
  // authenticate: sanitize at the sink — printable ASCII only — so a device-influenced
  // string (multipart filename, cmd fields) can never smuggle control bytes or markup
  // into the DB. Credential-bearing strings (otaUrl, MQTT_PASS, APN) must never be
  // %s'd into a log line at all — grep before adding one.
  { char *d = crashRing[crashHead]; size_t k = 0;
    for (const char *sp = buf; *sp && k < CRASH_LINE - 1; sp++) { unsigned char ch = (unsigned char)*sp; d[k++] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch : '?'; }
    d[k] = 0; }
  crashHead = (crashHead + 1) % CRASH_LINES;
  portENTER_CRITICAL(&logMux);
  strlcpy(logRing[logHead], buf, LOG_LINE);           // copy the string, not the stack bytes after its NUL
  logHead = (logHead + 1) % LOG_RING;
  if (logCount < LOG_RING) logCount++;
  portEXIT_CRITICAL(&logMux);
}

// ---------- device identity (NVS-backed) ----------
// deviceNameValid() and chipSeedName() live in deviceid.h (host-testable).

// ---------- reset forensics (diagnosing the sw-reset-instead-of-sleep loop) ----------
// RTC memory does NOT survive a software reset (only deep-sleep wakes), so the
// breadcrumb lives in NVS: each cycle stamps how far it got; the next boot reads
// where the previous one died. Phases:
//   1 = setup started (death here = modem power-on inrush brownout)
//   2 = LTE attached  (death here = RF-load brownout: publish/GNSS)
//   3 = modem work done (publish/gps complete)
//   4 = console window done, about to sleep
//   5 = goToSleep: teardown done (modem off)
//   6 = immediately before esp_deep_sleep_start (prev_ph==6 = previous cycle
//       completed fully; a brownout after that is the NEXT wake's inrush)
static const char *g_resetStr = "?";
static uint8_t prevPhase = 0;
// Consecutive BROWNOUT boots (NVS — RTC is wiped by every brownout). A sustained
// inrush loop means the cell/holder cannot recover between cycles; from the
// BROWNOUT_SHED_AFTER-th consecutive brownout the recovery cycle sheds GNSS (the
// longest RF load) so the cycle is light enough for the supply to settle, exactly
// as the AP is shed. Reset by the first clean deep-sleep wake.
#ifndef BROWNOUT_SHED_AFTER
#define BROWNOUT_SHED_AFTER 3
#endif
static uint8_t brownoutStreak = 0;
static void markPhase(uint8_t ph) {
  Preferences p;
  p.begin("freezermon", false);
  p.putUChar("ph", ph);
  p.end();
}

// Persist a name to NVS. Validates first, and reports the actual write result
// so callers never claim success on a failed write. Caller reboots so every
// MQTT topic / client id / payload tag re-derives cleanly.
static bool persistDeviceId(const char *name) {
  if (!deviceNameValid(name)) return false;
  Preferences prefs;
  prefs.begin("freezermon", false);
  size_t written = prefs.putString("devid", name);
  prefs.end();
  return written > 0;
}

// ---------- NVS-backed monitor state ----------
// The temp-alarm streak, movement anchor and AGPS age lived only in RTC memory,
// which every brownout wipes — on a marginal supply (the wake-inrush double-boot
// pattern) that meant the temp alarm could NEVER accumulate its 3 consecutive
// breaches, movement re-anchored every cycle, and AGPS re-downloaded every wake.
// Mirror them in NVS: written through on change (NVS skips writes when the value
// is unchanged, so liberal calls cost no flash wear), loaded back on any boot
// that wiped RTC. Deep-sleep wakes keep RTC as the live copy.
static void saveMonState() {
  Preferences p;
  p.begin("freezermon", false);
  p.putUChar("alrm",  alarmActive);
  p.putUChar("brch",  consecutiveBreaches);
  p.putFloat("alat",  anchorLat);
  p.putFloat("alon",  anchorLon);
  p.putUChar("mov",   movingActive);
  p.putUChar("still", stillStreak);
  p.putUChar("mvpd",  moveAlertPending);
  p.putUChar("tpnd",  tempAlertPending);
  p.putULong("agps",  lastAgpsEpoch);
  p.putUChar("rsg",   reportsSinceGps);   // GPS cadence counter: without this every brownout boot forced a full hunt
  p.putUChar("gzs",   gnssZeroSatStreak);
  p.putUChar("ccha",  coldChargeActive);  // one-shot latch: without this the cold_charge alert re-fired every brownout
  p.end();
}
static void loadMonState() {
  Preferences p;
  p.begin("freezermon", true);
  alarmActive         = p.getUChar("alrm", 0);
  consecutiveBreaches = p.getUChar("brch", 0);
  anchorLat           = p.getFloat("alat", 0);
  anchorLon           = p.getFloat("alon", 0);
  movingActive        = p.getUChar("mov", 0);
  stillStreak         = p.getUChar("still", 0);
  moveAlertPending    = p.getUChar("mvpd", 0);
  tempAlertPending    = p.getUChar("tpnd", 0);
  lastAgpsEpoch       = p.getULong("agps", 0);
  reportsSinceGps     = p.getUChar("rsg", 0xFF);
  gnssZeroSatStreak   = p.getUChar("gzs", 0);
  coldChargeActive    = p.getUChar("ccha", 0);
  p.end();
}

// Resolve deviceId once at boot: a valid stored NVS name wins; otherwise seed it
// — from the compile-time DEVICE_ID if that is itself a VALID name, else a
// unique-per-chip default so two un-named units never collide. The seed is
// validated so an invalid stored/compiled value can never keep re-seeding every
// wake (flash wear) or leak into topics; the chip default is always valid.
static void resolveDeviceId() {
  Preferences prefs;
  prefs.begin("freezermon", true);                 // read-only
  String stored = prefs.getString("devid", "");
  prefs.end();
  if (deviceNameValid(stored.c_str())) {
    strlcpy(deviceId, stored.c_str(), sizeof(deviceId));
    return;
  }
  char seed[32];
  if (deviceNameValid(DEVICE_ID)) {                // explicit, valid compiled-in name
    strlcpy(seed, DEVICE_ID, sizeof(seed));
  } else {                                          // derive a unique default from the NIC-unique MAC bytes
    chipSeedName(ESP.getEfuseMac(), seed, sizeof(seed));
  }
  strlcpy(deviceId, seed, sizeof(deviceId));        // use it this boot regardless of the NVS write
  Preferences w;
  w.begin("freezermon", false);
  bool ok = w.putString("devid", seed) > 0;
  w.end();
  logLine(ok ? "[id] seeded device name = %s" : "[id] seed name = %s (NVS write FAILED)", deviceId);
}

// ---------- helpers ----------
static bool awakeBudgetLeft() { return (millis() - awakeStart) < MAX_AWAKE_MS; }

// Supply-sag instrumentation (2.88). Every brownout dies at ph=1 — the modem power-on
// and boot. Sampling the battery ADC at ~500 Hz through those steps on the cycles
// that SURVIVE gives the depth and timing of the sag, hence the path resistance
// (R ≈ (rest − min) / I_pulse): the number that decides cell vs holder vs supercap.
RTC_DATA_ATTR uint16_t sagRestMv = 0, sagMinMv = 0, sagAtMs = 0, sagDeepN = 0; RTC_DATA_ATTR uint8_t sagStep = 0;
static uint32_t sagT0 = 0; static uint8_t sagCurStep = 0;
static void sagDelay(uint32_t ms) {               // delay() that watches the rail
  // Inrush spikes are sub-millisecond: sample back-to-back (~10 kHz) for the first
  // 60 ms after every edge, then every 2 ms. sagDeepN counts samples more than 300 mV
  // below rest — spike persistence, distinguishes a needle from a sustained sag.
  uint32_t start = millis(), end = start + ms;
  while ((int32_t)(end - millis()) > 0) {
    uint16_t v = analogReadMilliVolts(BOARD_BAT_ADC_PIN) * 2;
    if (v) {
      if (sagMinMv == 0 || v < sagMinMv) { sagMinMv = v; sagAtMs = (uint16_t)(millis() - sagT0); sagStep = sagCurStep; }
      if (sagRestMv && v + 300 < sagRestMv && sagDeepN < 65535) sagDeepN++;
    }
    if (millis() - start > 60) delay(2); else yield();
  }
}
static void modemPowerOn() {
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  // TRUE supply cut before powering up — not just a reset. The A76XX GNSS
  // engine can wedge (powered, CGNSSPWR READY, but tracking 0 satellites) and
  // only dropping the modem rail clears it; a reset/CPOF does not. Battery
  // wakes already lose this rail in deep sleep, but a continuously-powered
  // (external-supply) unit never does — so force it here on every power-on.
  digitalWrite(BOARD_POWERON_PIN, LOW);
  delay(1200);                                    // let the rail fully drain
  sagRestMv = analogReadMilliVolts(BOARD_BAT_ADC_PIN) * 2;   // resting voltage, modem rail off
  sagMinMv = 0; sagAtMs = 0; sagStep = 0; sagDeepN = 0; sagT0 = millis();
  // Precharge double-tap: slamming the drained rail on in one step draws an
  // inrush surge that browns out a marginal cell/holder (seen live 2026-07-15:
  // every wake's FIRST power-on died, the reboot's second attempt survived on
  // the now-precharged caps). Emulate that deliberately: a short first tap
  // charges the modem's bulk capacitance, the brief drop bounds the surge, and
  // the final enable then sees a much smaller di/dt. Costs 250 ms.
  sagCurStep = 1; digitalWrite(BOARD_POWERON_PIN, HIGH);   // tap 1: precharge the bulk caps
  sagDelay(150);
  digitalWrite(BOARD_POWERON_PIN, LOW);
  sagDelay(100);
  sagCurStep = 2; digitalWrite(BOARD_POWERON_PIN, HIGH);   // rail for modem section (caps precharged)
  agpsLoaded = false;                             // rail was cycled -> GNSS-engine RAM (and any AGPS data in it) is gone

  pinMode(MODEM_RESET_PIN, OUTPUT);               // reset pulse (LilyGO reference)
  sagCurStep = 3;
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); sagDelay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);  sagDelay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

  pinMode(BOARD_PWRKEY_PIN, OUTPUT);              // PWRKEY: 1 s active pulse (100 ms is flaky on A76xx)
  sagCurStep = 4;
  digitalWrite(BOARD_PWRKEY_PIN, LOW);  sagDelay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH); sagDelay(1000);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);
  sagCurStep = 5; sagDelay(3000);                 // modem boot: its own DC-DC + first RF — the deepest draw before attach

  // 8 KB RX ring buffer (vs the 256-byte default): headroom for the modem's
  // 4 KB HTTPREAD chunk to sit while Update.write() stalls on a flash-sector
  // erase. 8 KB (not 32 KB) keeps heap free for the OTA path. Must precede begin().
  SerialAT.setRxBufferSize(8192);
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
}

static bool modemConnect() {
  // Bounded AT handshake — modem needs a few seconds after PWRKEY
  bool up = false;
  for (int i = 0; i < 30 && awakeBudgetLeft(); i++) {   // ~30 s max
    if (modem.testAT(1000)) { up = true; break; }
  }
  if (!up) { logLine("[net] modem not answering AT"); return false; }
  logLine("[net] modem up");

  if (strlen(SIM_PIN) && modem.getSimStatus() != 1) modem.simUnlock(SIM_PIN);

#ifdef FORCE_PLMN
  // pin to the SIM's working carrier — auto-select can starve on MVNO SIMs.
  // Manual selection persists in modem NV, so skip the (slow) command when
  // the modem is already in manual mode — saves up to a minute of awake budget.
  modem.sendAT("+COPS?");
  String copsNow;
  modem.waitResponse(5000L, copsNow);
  if (copsNow.indexOf("+COPS: 1") < 0) {
    logLine("[net] pinning PLMN " FORCE_PLMN);
    modem.sendAT("+COPS=1,2,\"" FORCE_PLMN "\"");
    int r = modem.waitResponse(75000L);
    logLine("[net] PLMN pin result=%d", r);
  }
#endif

  // only start an attach attempt if it can complete inside the awake budget
  for (int attempt = 1;
       attempt <= CONNECT_ATTEMPTS &&
       (millis() - awakeStart) + NET_ATTACH_TIMEOUT_MS < MAX_AWAKE_MS;
       attempt++) {
    if (!modem.waitForNetwork(NET_ATTACH_TIMEOUT_MS)) {
      logLine("[net] attempt %d: no network registration", attempt);
    } else if (!modem.gprsConnect(APN, GPRS_USER, GPRS_PASS)) {
      logLine("[net] attempt %d: registered but PDP/APN failed", attempt);
    } else {
      logLine("[net] attached, IP %s", modem.getLocalIP().c_str());
      return true;
    }
    delay(attempt * 5000UL);                            // linear backoff, bounded
  }
  logLine("[net] out of attempts/awake budget");
  return false;
}

static uint32_t networkEpoch() {
  int y, mo, d, h, mi, s;
  float tz;
  if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &s, &tz)) return 0;
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
  time_t local = mktime(&t);                            // treat as UTC then correct tz
  // CCLK's zone field is QUARTER-HOURS (3GPP TS 27.007). The fork's A76xx
  // getNetworkTimeImpl returns it raw (skips the /4 the common impl does),
  // so "+08" arrives here as 8.0 meaning +2h. Seen live: frames stamped 6h
  // in the past (8h subtracted where 2h was right).
  return (uint32_t)(local - (time_t)(tz * 900.0f));
}

static void readSensors(Sample &s) {
  probes.begin();
  probes.requestTemperatures();
  float t0 = probes.getTempCByIndex(0);
  float t1 = probes.getTempCByIndex(1);
  s.tCab = (t0 == DEVICE_DISCONNECTED_C) ? NAN : t0;
  s.tAmb = (t1 == DEVICE_DISCONNECTED_C) ? NAN : t1;

  s.vbatMv   = analogReadMilliVolts(BOARD_BAT_ADC_PIN) * 2;    // 1:2 divider
  s.vsolarMv = analogReadMilliVolts(BOARD_SOLAR_ADC_PIN) * 2;
  s.extPower = s.vsolarMv >= EXT_POWER_MIN_MV ? 1 : 0;         // VIN present = external power

  pinMode(DOOR_PIN, INPUT_PULLUP);                      // reed closed (door shut) = LOW
  s.doorOpen = digitalRead(DOOR_PIN) == HIGH ? 1 : 0;
  s.ageS = 0;
  s.alarm = 0;
}

// ---------- field-debug console (WiFi AP, no telemetry role) ----------
// raw AT query helper for console pages (single-line, whitespace-flattened)
// Danish PLMN codes -> operator names. The modem reports numeric COPS
// (MCC 238 + MNC), which is meaningless when staring at a bench console.
static const char *plmnName(const String &plmn) {
  struct Plmn { const char *code, *name; };
  static const Plmn DK[] = {
    {"23801", "TDC / Nuuday"},
    {"23802", "Telenor DK"},
    {"23806", "3 DK (Hi3G)"},
    {"23812", "Lycamobile DK"},
    {"23820", "Telia DK"},
    {"23866", "TT-Netvaerket (Telia+Telenor shared RAN)"},
    {"23873", "Onomondo"},
  };
  for (auto &e : DK) if (plmn == e.code) return e.name;
  return "unknown";
}

static bool readGnssFix(float *outLat, float *outLon, int *satsUsed, float *hdopOut = nullptr, int *fixMode = nullptr, int *svs = nullptr);  // defined near maybeGps

static String atQuery(const char *cmd, uint32_t timeoutMs = 3000) {
  modem.sendAT(cmd);
  String r;
  modem.waitResponse(timeoutMs, r);
  r.replace("\r", " ");
  r.replace("\n", " ");
  r.replace("OK", "");
  r.trim();
  return r;
}

static bool gnssDebugOn = false;   // /gps keeps GNSS powered while console is up

static bool otaCheckSig(const uint8_t *digest, const uint8_t *sig, size_t siglen);  // defined below

// Local /update flash requires a valid signature too (same key as OTA), so the WPA2
// AP password is no longer the only barrier to installing arbitrary firmware. Upload
// state shared between the multipart upload callback and the completion handler.
static mbedtls_md_context_t upMd;
static uint8_t upDigest[32];
static uint8_t upSig[256];
static size_t  upSigLen  = 0;
static bool    upBinDone = false;
static bool    upFwBlocked = false;         // fw upload refused because an OTA was in flight
volatile bool  otaUploadActive = false;     // a local /update flash is in progress → block cellular OTA
volatile uint32_t otaUploadMs = 0;          // millis() when it started (staleness backstop)
// Serializes the two flash paths' claim: /update runs on the dbgweb task, performOta on
// the main thread. The claim (check the other's flag + set mine) must be atomic or both
// could Update.begin() at once and corrupt flash. The transfer itself isn't held under
// the lock — only the brief check-and-set.
static portMUX_TYPE flashMux = portMUX_INITIALIZER_UNLOCKED;

static void startDebugAp() {
  char ssid[48];
  snprintf(ssid, sizeof(ssid), "freezermon-%s", deviceId);
  WiFi.mode(WIFI_AP);
  // deviceNameValid() caps the name so "freezermon-<name>" fits the 32-char SSID
  // limit; if the radio still refuses the SSID, fall back to a safe fixed one so
  // /setname (the only field rename path) is never stranded.
  if (!WiFi.softAP(ssid, DEBUG_AP_PASSWORD)) WiFi.softAP("freezermon-setup", DEBUG_AP_PASSWORD);
  static const char *setnameHdr[] = { "Origin", "X-FreezerMon" };
  debugServer.collectHeaders(setnameHdr, 2);           // CSRF gate: browser Origin, or the provisioning header (curl)

  debugServer.on("/", []() {
    debugServer.send(200, "text/plain",
      "freezerMon debug console\n"
      "  /status  current readings + state (JSON)\n"
      "  /log     recent event log\n"
      "  /lte     modem/network status (registration, band, signal, IP, time)\n"
      "  /gps     GNSS debug (satellites, fix, GPS time)\n"
      "  /sms     SMS inbox (SIM activation texts etc.)\n"
      "  /setname set this unit's device name (stored in NVS, survives OTA)\n"
      "  /update  OTA firmware upload (browser form)\n");
  });
  debugServer.on("/status", []() {
    debugServer.sendHeader("Refresh", "15");            // browser auto-reload while monitoring
    // Served from the main thread's last reading: readSensors() bit-bangs the OneWire
    // bus and re-drives DOOR_PIN, and doing that from the console task while the main
    // thread samples corrupts DS18B20 conversions (NAN -> resets the breach streak).
    Sample s;
    if (lastSampleValid) s = lastSample; else readSensors(s);
    JsonDocument doc;
    doc["device"]    = deviceId;
    doc["fw"]        = FW_VERSION;
    doc["boot"]      = bootCount;
    doc["powered"]   = poweredSession;
    if (!isnan(s.tCab)) doc["t_cab"] = serialized(String(s.tCab, 2));
    if (!isnan(s.tAmb)) doc["t_amb"] = serialized(String(s.tAmb, 2));
    doc["door"]      = s.doorOpen;
    doc["ext_power"] = s.extPower;
    doc["vbat_mv"]   = s.vbatMv;
    doc["vsolar_mv"] = s.vsolarMv;
    doc["alarm"]     = alarmActive;
    doc["moving"]    = movingActive;
    doc["buffered"]  = rtcBufCount;
    doc["lat"]       = lastLat;
    doc["lon"]       = lastLon;
    doc["epoch_est"] = rtcEpoch;
    String out;
    serializeJsonPretty(doc, out);
    debugServer.send(200, "application/json", out);
  });
  debugServer.on("/log", []() {
    debugServer.sendHeader("Refresh", "15");            // browser auto-reload while monitoring
    String out;
    out.reserve(LOG_RING * LOG_LINE / 2);               // typical line is well under LOG_LINE; avoids most reallocs
    char line[LOG_LINE];
    portENTER_CRITICAL(&logMux);
    uint8_t count = logCount, head = logHead;
    portEXIT_CRITICAL(&logMux);
    for (uint8_t i = 0; i < count; i++) {
      portENTER_CRITICAL(&logMux);                       // one entry per critical section — never hold it across String work
      memcpy(line, logRing[(head + LOG_RING - count + i) % LOG_RING], LOG_LINE);
      portEXIT_CRITICAL(&logMux);
      line[LOG_LINE - 1] = 0;
      out += line;
      out += '\n';
    }
    debugServer.send(200, "text/plain", out);
  });
  debugServer.on("/lte", []() {
    debugServer.sendHeader("Refresh", "15");            // browser auto-reload while monitoring
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    if (poweredSession) {
      // powered regime: main loop services MQTT on the UART continuously —
      // report link state without injecting AT commands into that stream
      doc["powered"]        = true;
      doc["mqtt_connected"] = mqtt.connected();
      doc["note"] = "live modem queries suspended while MQTT session is held";
    } else if (ConsoleModemHold hold; !hold.held) {
      // main thread owns the modem UART right now (attach/publish/OTA in progress)
      doc["busy"] = true;
      doc["note"] = "modem attach/publish in progress - refresh in ~30s";
    } else {
      doc["sim_status"] = (int)modem.getSimStatus();      // 1 = ready
      int csq = modem.getSignalQuality();
      doc["csq"] = csq;
      if (csq >= 0 && csq < 99) doc["rssi_dbm"] = -113 + 2 * csq;
      doc["registered"]  = modem.isNetworkConnected();
      String op = modem.getOperator();
      doc["operator"]      = op;
      doc["operator_name"] = plmnName(op);
      doc["data_attach"] = modem.isGprsConnected();
      doc["ip"]          = modem.getLocalIP();
      doc["ccid"]        = modem.getSimCCID();
      doc["imei"]        = modem.getIMEI();
      // radio detail: system mode, PLMN, cell, band, RSRP/RSRQ/SINR
      doc["radio"]       = atQuery("+CPSI?");
      doc["reg_lte"]     = atQuery("+CEREG?");            // LTE (EPS) registration
      doc["reg_gsm"]     = atQuery("+CREG?");             // 2G/CS registration
      doc["network_time"] = atQuery("+CCLK?");            // clock as set by the network

      // ---- assigned-IP / PDP context detail (APN, IP, subnet, gateway, DNS) ----
      doc["apn_cfg"]     = APN;                           // compiled-in APN
      doc["pdp_addr"]    = atQuery("+CGPADDR");           // context IP address(es)
      doc["pdp_dhcp"]    = atQuery("+CGCONTRDP");         // APN, local IP + subnet mask, gateway, DNS1, DNS2
      doc["pdp_active"]  = atQuery("+CGACT?");            // context activation state
      doc["pdp_define"]  = atQuery("+CGDCONT?");          // configured contexts / APNs
      doc["dns_lookup"]  = atQuery("+CDNSGIP=\"" MQTT_HOST "\"", 5000);  // resolve via the network-provided DNS
    }                                                     // hold released by the guard
    doc["mqtt_host"] = MQTT_HOST;
    doc["mqtt_port"] = MQTT_PORT;
    String out;
    serializeJsonPretty(doc, out);
    debugServer.send(200, "application/json", out);
  });

  debugServer.on("/gps", []() {
    debugServer.sendHeader("Refresh", "15");            // browser auto-reload while monitoring
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    if (poweredSession) {
      doc["powered"] = true;
      doc["note"] = "GNSS debug unavailable while MQTT session is held (UART owned by main loop)";
      doc["last_lat"] = lastLat;
      doc["last_lon"] = lastLon;
    } else if (ConsoleModemHold hold; !hold.held) {
      doc["busy"] = true;
      doc["note"] = "modem attach/publish in progress - refresh in ~30s";
    } else {
      if (!gnssDebugOn)                                   // stays on while console is up
        gnssDebugOn = modem.enableGPS(GPS_ANTENNA_POWER_PIN, GPS_ANTENNA_POWER_LEVEL);
      doc["gnss_powered"] = gnssDebugOn;
      float lat = 0, lon = 0, hdop = 0;
      int sats = 0, mode = 0;
      if (readGnssFix(&lat, &lon, &sats, &hdop, &mode)) { // anchor-based parse — fork's getGPS misreads this modem
        doc["fix"] = true;
        doc["lat"] = lat;
        doc["lon"] = lon;
        doc["sats"] = sats;
        doc["mode"] = mode;                                // 2 = 2D, 3 = 3D
        doc["hdop"] = serialized(String(hdop, 1));
        doc["quality_ok"] = (mode >= 3 || !GPS_REQUIRE_3D) && hdop > 0 && hdop <= GPS_MAX_HDOP;
        doc["raw"] = atQuery("+CGNSSINFO");
        // deliberately NOT written to lastLat/lastLon: a console-triggered fix is not
        // a report and must not shift the movement-detection baseline
      } else {
        doc["fix"] = false;
        String raw = atQuery("+CGNSSINFO");
        // satellites are visible before the fix is:
        // +CGNSSINFO: [<mode>],[GPS-SVs],[BEIDOU-SVs],[GLONASS-SVs],[GALILEO-SVs],...
        int colon = raw.indexOf("+CGNSSINFO:");
        if (colon >= 0) {
          String body = raw.substring(colon + 11);
          int svs[4] = {0, 0, 0, 0};
          int from = body.indexOf(',') + 1;              // skip <mode> (empty pre-fix)
          for (int f = 0; f < 4 && from > 0; f++) {
            int next = body.indexOf(',', from);
            if (next < 0) break;
            svs[f] = body.substring(from, next).toInt(); // empty field -> 0
            from = next + 1;
          }
          doc["sats_gps"]     = svs[0];
          doc["sats_beidou"]  = svs[1];
          doc["sats_glonass"] = svs[2];
          doc["sats_galileo"] = svs[3];
          doc["sats_visible"] = svs[0] + svs[1] + svs[2] + svs[3];
        }
        doc["raw"] = raw;
        doc["wedge_streak"] = gnssZeroSatStreak;          // 0-sat cycles; >= GNSS_STUCK_CYCLES forces a power cycle
        doc["note"] = "no fix yet - first fix needs 30-120s with sky view; refresh";
      }
      doc["last_reported_lat"] = lastLat;
      doc["last_reported_lon"] = lastLon;
    }                                                     // hold released by the guard
    String out;
    serializeJsonPretty(doc, out);
    debugServer.send(200, "application/json", out);
  });

  debugServer.on("/sms", []() {
    // SIM activation flows deliver a text — surface the inbox here so no
    // phone is needed to complete registration in the field. Same UART rules
    // as /lte: never inject AT commands into a session the main thread owns.
    if (poweredSession) {
      debugServer.send(503, "text/plain", "unavailable while the MQTT session is held (powered regime)\n");
      return;
    }
    ConsoleModemHold hold;
    if (!hold.held) {
      debugServer.send(503, "text/plain", "modem busy (attach/publish/OTA in progress) - retry in ~30s\n");
      return;
    }
    modem.sendAT("+CMGF=1");                  // text mode
    modem.waitResponse();
    modem.sendAT("+CMGL=\"ALL\"");            // list all stored messages
    String res;
    modem.waitResponse(10000L, res);
    res.replace("\r", "");
    debugServer.send(200, "text/plain",
                     res.length() > 4 ? res : "no SMS stored\n");
  });

  // Rename this unit from the field console. The name is stored in NVS (survives
  // OTA) and drives the MQTT topics + the InfluxDB `device` tag, so one firmware
  // image serves a fleet — flash, then name each unit here. Reboots to re-derive.
  // GET shows the form; the rename itself is POST-only and same-origin-checked,
  // so a drive-by page loaded on the AP can't rename+reboot the unit via a bare
  // <img src=".../setname?name=x"> (CSRF). A rename to the current name is a
  // no-op so it can't be scripted into a reboot loop.
  debugServer.on("/setname", HTTP_GET, []() {
    debugServer.send(200, "text/html",
      "<h3>Device name</h3>current: <b>" + String(deviceId) + "</b>"
      "<form method='POST' action='/setname'>"
      "new name: <input name='name' required> "
      "<input type='submit' value='Set + reboot'></form>"
      "<small>lowercase letters, digits, hyphen; 1-21 chars. "
      "Changes the MQTT topics and the Grafana device tag.</small>");
  });
  debugServer.on("/setname", HTTP_POST, []() {
    if (!sameOriginRequest()) {
      debugServer.send(403, "text/plain", "cross-origin request refused\n");
      return;
    }
    String name = debugServer.arg("name");
    if (name == String(deviceId)) {                    // unchanged -> no NVS write, no reboot
      debugServer.send(200, "text/plain", "name unchanged\n");
      return;
    }
    if (!persistDeviceId(name.c_str())) {
      debugServer.send(400, "text/plain", "invalid name - use [a-z0-9-], 1-21 chars\n");
      return;
    }
    debugServer.send(200, "text/plain", "renamed to " + name + " - rebooting\n");
    delay(400);
    ESP.restart();
  });

  // OTA over the debug AP: upload a new firmware.bin from any browser at
  // /update — no wires, no cellular data. Dual OTA partitions handle rollback
  // space; a failed write leaves the running slot untouched.
  debugServer.on("/update", HTTP_GET, []() {
    debugServer.send(200, "text/html",
      "<h3>freezerMon OTA (fw " FW_VERSION ")</h3>"
      // ver goes in the QUERY STRING (via onsubmit), not a multipart field: the ESP32
      // WebServer only merges multipart fields into _currentArgs AFTER the upload
      // callback runs, so arg("ver") would be empty during hashing. URL query args ARE
      // available in the callback.
      "<form method='POST' action='/update' enctype='multipart/form-data' "
      "onsubmit=\"this.action='/update?ver='+encodeURIComponent(document.getElementById('v').value)\">"
      "version (e.g. 2.51): <input id='v' required><br>"   // id, not name: no multipart field to shadow the ?ver= query arg
      "firmware.bin: <input type='file' name='fw' accept='.bin'><br>"
      "firmware.bin.sig: <input type='file' name='sig' accept='.sig'><br>"
      "<input type='submit' value='Flash (signed only)'></form>"
      "<small>version + both files required — installs only if the signature (over version+image) verifies</small>"
#ifdef OTA_MANIFEST_URL
      "<hr><form method='POST' action='/update/check'>"
      "<input type='submit' value='Check online for update'> "
      "fetches the published manifest over LTE and installs if newer</form>"
#endif
      );
  });
  debugServer.on("/update/check", HTTP_POST, []() {
    if (!sameOriginRequest()) { debugServer.send(403, "text/plain", "cross-origin request refused\n"); return; }
#ifdef OTA_MANIFEST_URL
    otaCheckRequested = true;
    debugServer.send(200, "text/plain",
      "queued - the device fetches the manifest at its next modem-free moment; watch /log\n");
#else
    debugServer.send(503, "text/plain", "OTA_MANIFEST_URL not set in config.h\n");
#endif
  });
  debugServer.on("/update", HTTP_POST, []() {
    if (!sameOriginRequest()) {                          // upload callback already refused; make the answer explicit
      Update.abort();
      upBinDone = false; upSigLen = 0; otaUploadActive = false; upFwBlocked = false;
      debugServer.send(403, "text/plain", "cross-origin request refused\n");
      return;
    }
    // Both files in; install ONLY if the uploaded image's SHA-256 verifies against the
    // uploaded signature and the embedded public key — then commit and reboot.
    bool ok = !upFwBlocked && upBinDone && upSigLen == 256 && !Update.hasError() &&
              otaCheckSig(upDigest, upSig, 256) && Update.end(true);
    if (!ok) Update.abort();
    const char *msg = upFwBlocked ? "REJECTED - cellular OTA in progress, try again shortly\n"
                    : ok           ? "OK - signature valid, rebooting\n"
                                   : "REJECTED - missing/invalid signature, still on old firmware\n";
    upBinDone = false; upSigLen = 0; otaUploadActive = false; upFwBlocked = false;
    debugServer.send(200, "text/plain", msg);
    logLine("[ota] /update signed-flash ok=%d", ok);
    delay(500);
    if (ok) ESP.restart();
  }, []() {
    HTTPUpload &up = debugServer.upload();
    bool isSig = up.name == "sig";
    if (up.status == UPLOAD_FILE_START) {
      // Checked here, not only in the POST handler: the partition erase (Update.begin)
      // happens in this callback, before the handler ever runs.
      if (!sameOriginRequest()) { upFwBlocked = true; logLine("[ota] /update refused - cross-origin"); return; }
      logLine("[ota] receiving %.16s (%.8s)", up.filename.c_str(), up.name.c_str());   // capped: these strings reach crash_log
      if (isSig) { upSigLen = 0; }
      else {
        upBinDone = false;
        if (otaUploadActive) mbedtls_md_free(&upMd);    // defensive: a prior upload dropped mid-stream
        // Atomically claim exclusive flash access vs. the cellular OTA (main thread).
        portENTER_CRITICAL(&flashMux);
        bool busy = modemBusy;
        if (!busy) { otaUploadActive = true; otaUploadMs = millis(); }
        portEXIT_CRITICAL(&flashMux);
        if (busy) { upFwBlocked = true; logLine("[ota] /update refused - modem busy"); return; }
        upFwBlocked = false;
        // ver comes from the URL query (?ver=), available in this callback (unlike a
        // multipart field). Empty ver → hash won't match the operator's sig → reject.
        String v = debugServer.arg("ver");
        if (v.length() == 0) { upFwBlocked = true; logLine("[ota] /update refused - missing ver"); return; }
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { upFwBlocked = true; Update.abort(); logLine("[ota] /update begin fail"); return; }
        mbedtls_md_init(&upMd);
        if (mbedtls_md_setup(&upMd, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
            mbedtls_md_starts(&upMd) != 0) {
          upFwBlocked = true; mbedtls_md_free(&upMd); Update.abort(); otaUploadActive = false;
          logLine("[ota] /update sha init fail"); return;
        }
        // Bind the version into the hash, same as OTA (sig is over ver‖0x00‖image).
        uint8_t z = 0;
        mbedtls_md_update(&upMd, (const uint8_t *)v.c_str(), v.length());
        mbedtls_md_update(&upMd, &z, 1);
        logLine("[ota] /update ver=%.15s", v.c_str());
      }
    } else if (up.status == UPLOAD_FILE_WRITE) {
      if (isSig) {                                 // keep only the first 256 bytes (real sig; rest is padding)
        for (size_t i = 0; i < up.currentSize && upSigLen < 256; i++) upSig[upSigLen++] = up.buf[i];
      } else if (!upFwBlocked) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
          upFwBlocked = true; mbedtls_md_free(&upMd); logLine("[ota] /update write fail");   // POST handler rejects + aborts
        } else {
          mbedtls_md_update(&upMd, up.buf, up.currentSize);
        }
      }
      esp_task_wdt_reset();                        // uploads outlast the WDT window
    } else if (up.status == UPLOAD_FILE_END) {
      if (!isSig && !upFwBlocked) {
        mbedtls_md_finish(&upMd, upDigest);
        mbedtls_md_free(&upMd);
        upBinDone = true;                          // do NOT Update.end here — the POST handler gates on the signature
        // keep otaUploadActive until the POST handler commits/aborts, or the
        // OTA task could Update.begin() between here and the commit
        logLine("[ota] image received %u bytes - awaiting sig check", up.totalSize);
      }
    } else if (up.status == UPLOAD_FILE_ABORTED) {
      if (!isSig && !upFwBlocked) { Update.abort(); mbedtls_md_free(&upMd); }
      upBinDone = false; upSigLen = 0; otaUploadActive = false;
      logLine("[ota] upload aborted");
    }
  });

  debugServer.begin();
  debugApActive = true;

  // Serve the console from its own task so it answers even while the main
  // thread is deep inside a blocking LTE attach (the moment you need it most).
  static bool taskStarted = false;
  if (!taskStarted) {
    taskStarted = true;
    xTaskCreatePinnedToCore([](void *) {
      for (;;) {
        if (debugApActive) debugServer.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }, "dbgweb", 6144, nullptr, 1, nullptr, 0);
  }
  logLine("[debug] AP %s up - http://192.168.4.1", ssid);
}

// ---------- OTA transport ----------
// Field evidence on the A7608: the FIRST TLS connect after modem power-on
// always succeeds (that's the MQTT session); every later CCHOPEN in the same
// power cycle fails, on either mux, with or without CCHSTOP in between. So:
// try the cheap direct connect once, and when it fails fall back to the one
// path that provably works — power-cycle the modem, reattach, connect fresh.
static bool otaConnect(const char *host, uint16_t port) {
  // A second TLS session in the same power cycle is unreliable on the A7608's
  // CCH stack. Retry with a clean SSL-service teardown between tries. NEVER
  // power-cycle the modem here: doing so left it unresponsive ("modem not
  // answering AT") and killed the session MQTT still needed. OTA is
  // best-effort — the manual WiFi /update form is the guaranteed path.
  for (int attempt = 1; attempt <= 3 && awakeBudgetLeft(); attempt++) {
    if (otaHttpClient.connect(host, port)) return true;
    logLine("[ota] connect attempt %d failed: %s", attempt, atQuery("+CCHOPEN?").c_str());
    otaHttpClient.stop();                 // CCHCLOSE/CCHSTOP the SSL service
    delay(3000);
    esp_task_wdt_reset();
  }
  return false;
}

// ---------- forced online update check (/update "Check online" button) ----------
// Fetches OTA_MANIFEST_URL ({"ota_ver":"x.y","ota_url":"https://.../firmware.bin"})
// and arms the normal OTA path when the manifest names a different version.
// Shares netClient with MQTT, so the session is dropped first — callers restore it.
#ifdef OTA_MANIFEST_URL
static void checkOnlineUpdate() {
  otaCheckRequested = false;
  const char *url = OTA_MANIFEST_URL;
  bool tls = strncmp(url, "https://", 8) == 0;
  if (!tls && strncmp(url, "http://", 7) != 0) return;
  const char *p = url + (tls ? 8 : 7);
  const char *slash = strchr(p, '/');
  if (!slash || (size_t)(slash - p) >= 96) return;
  char host[96], path[160];
  memcpy(host, p, slash - p);
  host[slash - p] = 0;
  strlcpy(path, slash, sizeof(path));

  uint16_t port = tls ? 443 : 80;
  char *colon = strchr(host, ':');
  if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

  logLine("[ota] manifest check %s:%u", host, port);
  bool priorBusy = modemBusy;                           // restore, don't clear: the caller may own the session
  mainTakeModem();
  modemBusy = true;
  bool up = otaConnect(host, port);
  if (!up) {
    logLine("[ota] manifest connect failed");
    modemBusy = priorBusy; mainGiveModem();
    return;
  }
  otaHttpClient.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
  otaHttpClient.setTimeout(15000);
  String status = otaHttpClient.readStringUntil('\n');
  if (status.indexOf("200") < 0) {
    logLine("[ota] manifest http %s", status.c_str());
    otaHttpClient.stop();
    modemBusy = priorBusy; mainGiveModem();
    return;
  }
  while (true) {                                        // skip headers
    String h = otaHttpClient.readStringUntil('\n');
    h.trim();
    if (!h.length()) break;
  }
  String bodyStr;
  uint32_t t0 = millis();
  while ((otaHttpClient.connected() || otaHttpClient.available()) &&
         millis() - t0 < 10000UL && bodyStr.length() < 512) {
    while (otaHttpClient.available() && bodyStr.length() < 512) bodyStr += (char)otaHttpClient.read();
    delay(10);
  }
  otaHttpClient.stop();
  modemBusy = priorBusy; mainGiveModem();

  JsonDocument doc;
  if (deserializeJson(doc, bodyStr)) {
    logLine("[ota] manifest parse failed");
    return;
  }
  const char *u = doc["ota_url"], *v = doc["ota_ver"];
  if (!u || !v) {
    logLine("[ota] manifest missing ota_url/ota_ver");
    return;
  }
  if (verNewer(v, FW_VERSION)) {                     // anti-rollback: newer only
    if (!otaUrlSafe(u)) { logLine("[ota] manifest: rejected unsafe url"); return; }
    strlcpy(otaUrl, u, sizeof(otaUrl));
    strlcpy(otaVer, v, sizeof(otaVer));
    otaSize = doc["ota_size"] | 0L;                  // set the full pull metadata, not just
    otaSkip = doc["ota_skip"] | 4096L;               // url/ver — else performOta runs on stale
    { const char *m = doc["ota_md5"]; strlcpy(otaMd5, m ? m : "", sizeof(otaMd5)); }
    otaPending = true;
    logLine("[ota] manifest: v%s available (running " FW_VERSION ")", v);
  } else {
    logLine("[ota] manifest: already on " FW_VERSION);
  }
}
#endif

// ---------- OTA over LTE: modem-filesystem download (robust) ----------
// The ESP cannot reliably read a streamed HTTP body out of the modem's CCH TCP
// stack (confirmed live: empty/partial reads, no keep-alive, no reconnect, a
// ~340 KB wall). So the MODEM downloads the .bin into its OWN filesystem via
// AT+HTTPREADFILE, then the ESP reads the file back out locally over the UART in
// chunks with AT+CFTRANTX and flashes it — no network streaming on the ESP side.
// Approach from LilyGo-Modem-Series issue #443 (flowjob1), adapted.
static void otaDrain(Stream &s) { while (s.available()) s.read(); }

static bool otaWaitToken(Stream &s, const char *tok, uint32_t timeoutMs) {
  size_t len = strlen(tok), matched = 0;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (s.available()) {
      char c = s.read();
      if (c == tok[matched]) { if (++matched == len) return true; }
      else                   matched = (c == tok[0]) ? 1 : 0;
    }
    delay(1);
  }
  return false;
}

static bool otaReadLine(Stream &s, char *out, size_t sz, uint32_t timeoutMs) {
  uint32_t start = millis(); size_t i = 0;
  while (millis() - start < timeoutMs) {
    while (s.available()) {
      char c = s.read();
      if (c == '\r') continue;
      if (c == '\n') { if (!i) continue; out[i] = 0; return true; }
      if (i < sz - 1) out[i++] = c;
    }
    delay(1);
  }
  out[0] = 0; return false;
}

static bool otaReadRaw(Stream &s, uint8_t *dst, size_t n, uint32_t idleMs) {
  size_t got = 0; uint32_t last = millis();
  while (got < n) {
    int avail = s.available();
    if (avail <= 0) { if (millis() - last > idleMs) return false; delay(1); continue; }
    size_t want = ((size_t)avail < n - got) ? (size_t)avail : (n - got);
    size_t r = s.readBytes(dst + got, want);
    if (r) { got += r; last = millis(); esp_task_wdt_reset(); }
  }
  return true;
}

// Verify a detached RSA-2048/SHA-256 signature (256 bytes) over an image's SHA-256
// `digest`, against the public key baked into firmware. THE gate to installing any
// firmware — used by both cellular OTA and the local /update flash. Unforgeable
// without the private key, so it holds even over unauthenticated channels.
static bool otaCheckSig(const uint8_t *digest, const uint8_t *sig, size_t siglen) {
  if (siglen != 256) { logLine("[ota] sig len %u", (unsigned)siglen); return false; }
  mbedtls_pk_context pk; mbedtls_pk_init(&pk);
  int r = mbedtls_pk_parse_public_key(&pk, (const unsigned char *)OTA_PUBKEY_PEM, sizeof(OTA_PUBKEY_PEM));
  if (r != 0) { logLine("[ota] pubkey parse err %d", r); mbedtls_pk_free(&pk); return false; }
  r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, 256);
  mbedtls_pk_free(&pk);
  if (r != 0) { logLine("[ota] BAD SIGNATURE (%d) - refusing install", r); return false; }
  logLine("[ota] signature verified");
  return true;
}

// Fetch <url>.sig and verify it against the SHA-256 `digest` of the assembled image.
// The .sig file is 256 real bytes + zero padding: the modem drops the last ~3072
// bytes of EVERY cached response, so a bare 256-byte file can't be read out at all.
// We read only the FIRST 256 bytes (the signature); the padding absorbs the drop.
static bool otaVerifySig(Stream &at, const char *url, const uint8_t *digest) {
  char purl[224]; snprintf(purl, sizeof(purl), "%s.sig", url);
  char line[128];
  uint8_t sig[256]; long got = 0;
  for (int tries = 1; tries <= 3 && got != 256; tries++) {
    got = 0;
    esp_task_wdt_reset();
    at.println("AT+HTTPTERM"); otaWaitToken(at, "OK", 3000); otaDrain(at);
    at.println("AT+HTTPINIT");
    if (!otaWaitToken(at, "OK", 8000)) { delay(500); continue; }
    at.print("AT+HTTPPARA=\"URL\",\""); at.print(purl); at.println("\"");
    if (!otaWaitToken(at, "OK", 5000)) { delay(500); continue; }
    at.println("AT+HTTPACTION=0");
    if (!otaWaitToken(at, "OK", 5000)) { delay(500); continue; }
    int rc = 0; long plen = 0; bool ga = false; uint32_t start = millis();
    while (millis() - start < 30000UL) {
      if (!otaReadLine(at, line, sizeof(line), 3000)) continue;
      if (sscanf(line, "+HTTPACTION: 0,%d,%ld", &rc, &plen) == 2) { ga = true; break; }
    }
    // padded file: expect plen >= 256; we only read the first 256 (the real sig)
    if (!ga || rc != 200 || plen < 256) { logLine("[ota] sig http %d len %ld", rc, plen); delay(500); continue; }
    bool rok = true;
    while (got < 256 && rok) {
      at.printf("AT+HTTPREAD=%ld,%ld\r\n", got, 256 - got);
      long cg = 0; start = millis(); bool endMk = false;
      while (!endMk && millis() - start < 8000UL) {
        if (!otaReadLine(at, line, sizeof(line), 3000)) continue;
        char *h = strstr(line, "+HTTPREAD:");
        if (!h) { if (strstr(line, "ERROR")) { rok = false; break; } continue; }
        long n = 0;
        if (sscanf(h, "+HTTPREAD: DATA,%ld", &n) != 1) sscanf(h, "+HTTPREAD: %ld", &n);
        if (n <= 0) { endMk = true; break; }
        if (n > 256 - got) n = 256 - got;
        if (!otaReadRaw(at, sig + got, n, 4000)) { rok = false; break; }
        got += n; cg += n; start = millis();
      }
      if (!rok || cg == 0) { rok = false; break; }
    }
  }
  at.println("AT+HTTPTERM"); otaWaitToken(at, "OK", 3000);
  if (got != 256) { logLine("[ota] sig fetch failed"); return false; }
  return otaCheckSig(digest, sig, 256);
}

#ifndef OTA_MAX_FAILS
#define OTA_MAX_FAILS 3        // refuse a version after this many failed pulls (until a different version is commanded)
#endif
#ifndef MAX_OTA_MS
#define MAX_OTA_MS 900000UL    // hard cap on one OTA pull (15 min) — the 3-min awake guard is suspended during OTA
#endif
// The retained cmd is re-delivered on every subscribe, so without a memory of
// failures a permanently-bad image (stalling server, wrong size, bad signature)
// re-runs the full multi-minute pull on every wake — an unbounded battery drain,
// remotely triggerable by anyone who can write the cmd topic (2026-08-26 review).
static void otaNoteFailure(const char *ver) {
  Preferences p; p.begin("freezermon", false);
  String fv = p.getString("otafv", ""); uint8_t fc = p.getUChar("otafc", 0);
  if (fv != ver) { fc = 0; p.putString("otafv", ver); }
  if (fc < 255) fc++;
  uint8_t tc = p.getUChar("otatc", 0); if (tc < 255) tc++;
  p.putUChar("otafc", fc); p.putUChar("otatc", tc); p.end();
  logLine("[ota] %s failure #%u recorded (%u consecutive overall)", ver, fc, tc);
}
#ifndef OTA_MAX_FAILS_TOTAL
#define OTA_MAX_FAILS_TOTAL 6    // consecutive failed pulls across ANY version labels; only a successful install resets it
#endif
static bool otaRefused(const char *ver) {
  Preferences p; p.begin("freezermon", true);
  String fv = p.getString("otafv", ""); uint8_t fc = p.getUChar("otafc", 0); uint8_t tc = p.getUChar("otatc", 0); p.end();
  if (fv == ver && fc >= OTA_MAX_FAILS) { logLine("[ota] %s refused - failed %u times", ver, fc); return true; }
  // Per-version memory alone is evadable by cycling ota_ver labels (review 2026-08-27):
  // a global consecutive counter closes that; it is zeroed at boot when FW_VERSION
  // differs from the version recorded at the previous boot, i.e. after a real install.
  if (tc >= OTA_MAX_FAILS_TOTAL) { logLine("[ota] refused - %u consecutive failed pulls, waiting for a successful install", tc); return true; }
  return false;
}

static bool performOta(const char *url, const char *ver) {
  if (otaRefused(ver)) return false;
  bool priorBusy = modemBusy;                  // restore, don't clear: the caller may still own the session
  // Atomically claim exclusive flash access vs. a local /update flash (dbgweb task).
  // A stale otaUploadActive (dropped upload) is ignored after 3 min.
  portENTER_CRITICAL(&flashMux);
  bool busy = otaUploadActive && (millis() - otaUploadMs < 180000UL);
  if (!busy) modemBusy = true;                 // claim it before releasing the lock
  portEXIT_CRITICAL(&flashMux);
  if (busy) { logLine("[ota] deferred - local /update flash in progress"); return false; }
  mainTakeModem();
  const uint32_t otaStart = millis();
  Stream &at = SerialAT;
  const long RD = 4096;                  // one HTTPREAD chunk: the modem sends 4 KB
                                         // then waits for the next command, so it
                                         // never bursts past what we can drain — no
                                         // UART overflow during Update.write() stalls.
  long total = otaSize;                  // total image size, from the retained cmd
  modemBusy = true;
  logLine("[ota] %s size %ld heap %u (pieces)", ver, total, ESP.getFreeHeap());
  if (total <= 0) { logLine("[ota] no size in cmd"); otaNoteFailure(ver); modemBusy = priorBusy; mainGiveModem(); return false; }
  // Free the AT channel of the MQTT/CCH session (separate modem service).
  if (mqtt.connected()) mqtt.disconnect();
  delay(300);
  otaDrain(at);

  char line[128];
  static uint8_t pieceBuf[32768];        // whole-piece staging buffer (want <= ~28 KB): a
                                         // piece is committed to flash only after it reads
                                         // cleanly end-to-end, so a failed read is re-fetched
                                         // from offset 0 — the modem's offset-resume after a
                                         // re-fetch is unreliable and corrupted the image.
  if (!Update.begin(total)) { logLine("[ota] begin fail heap %u", ESP.getFreeHeap()); otaNoteFailure(ver); modemBusy = priorBusy; mainGiveModem(); return false; }
  if (otaMd5[0]) Update.setMD5(otaMd5);  // verify the whole image; never install a corrupt one

  // Stream SHA-256 over (ota_ver ‖ 0x00 ‖ image) — the version is part of the signed
  // payload, so verNewer() below plus this binding stop a MITM from relabelling an old
  // signed image with a higher version to force a downgrade.
  mbedtls_md_context_t md; mbedtls_md_init(&md);
  if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
      mbedtls_md_starts(&md) != 0) {
    logLine("[ota] sha init fail"); mbedtls_md_free(&md); Update.abort(); otaNoteFailure(ver); modemBusy = priorBusy; mainGiveModem(); return false;
  }
  { uint8_t z = 0;
    mbedtls_md_update(&md, (const uint8_t *)ver, strlen(ver));   // ver == otaVer (from cmd)
    mbedtls_md_update(&md, &z, 1); }

  // Fetch the image piece by piece: <url>.000, .001, … Each piece is a small file
  // the modem pulls as a plain 200 GET. We stage each piece FULLY in RAM and only
  // Update.write() it after a clean end-to-end read. A failed read re-fetches and
  // re-reads the piece FROM OFFSET 0 — never a non-zero offset: the modem's
  // offset-resume after a re-fetch returns wrong bytes (byte count still hits 100%
  // but the image checksum fails). Up to MAX_TRIES per piece so a transient blip
  // can't abort the pull; the whole image is MD5-verified at Update.end.
  long done = 0, lastLog = 0; int piece = 0; bool ok = true;
  const int MAX_TRIES = 10;
  // Let the modem's HTTP app settle after the MQTT/CCH teardown before the first
  // fetch — the first GET right after attach is the flakiest (piece 0 historically
  // eats the most retries). A one-time pause here converges the pull faster.
  delay(2500);
  // Time-boxed: this is the one long path that feeds the watchdog on every retry, so
  // without its own budget a stalling server keeps the radio hot for hours per wake.
  while (done < total && ok && millis() - otaStart < MAX_OTA_MS) {
    char purl[224];
    snprintf(purl, sizeof(purl), "%s.%03d", url, piece);
    long pieceStart = done;                      // flash position where this piece begins
    bool pieceOk = false;
    for (int tries = 1; tries <= MAX_TRIES && !pieceOk && ok && millis() - otaStart < MAX_OTA_MS; tries++) {
      esp_task_wdt_reset();
      at.println("AT+HTTPTERM"); otaWaitToken(at, "OK", 3000); otaDrain(at);
      at.println("AT+HTTPINIT");
      if (!otaWaitToken(at, "OK", 8000)) { logLine("[ota] init fail p%d t%d", piece, tries); delay(600); continue; }
      at.print("AT+HTTPPARA=\"URL\",\""); at.print(purl); at.println("\"");
      if (!otaWaitToken(at, "OK", 5000)) { logLine("[ota] url fail p%d t%d", piece, tries); delay(600); continue; }
      at.println("AT+HTTPACTION=0");
      if (!otaWaitToken(at, "OK", 5000)) { logLine("[ota] action fail p%d t%d", piece, tries); delay(600); continue; }
      int rc = 0; long plen = 0; bool ga = false; uint32_t start = millis();
      while (millis() - start < 60000UL) {
        if (!otaReadLine(at, line, sizeof(line), 3000)) continue;
        if (sscanf(line, "+HTTPACTION: 0,%d,%ld", &rc, &plen) == 2) { ga = true; break; }
      }
      if (!ga || rc != 200 || plen <= 0) { logLine("[ota] p%d http %d len %ld t%d", piece, rc, plen, tries); delay(600); continue; }

      // Read only the deliverable prefix — min(plen - otaSkip, remaining image). The
      // server overlaps pieces by otaSkip (the modem drops each response's last ~3072
      // bytes), so these prefixes tile the image with no gap.
      long want = plen - otaSkip;
      if (total - pieceStart < want) want = total - pieceStart;   // last piece: real remainder
      if (want <= 0 || (size_t)want > sizeof(pieceBuf)) { logLine("[ota] p%d bad want %ld", piece, want); ok = false; break; }

      // Stage `want` bytes into pieceBuf, always from offset 0. Each HTTPREAD emits
      // one or more "+HTTPREAD: <n>" chunks ending in "+HTTPREAD: 0" — read them all.
      long got = 0; bool readOk = true;
      while (got < want && readOk) {
        long sub = (want - got < RD) ? (want - got) : RD;
        at.printf("AT+HTTPREAD=%ld,%ld\r\n", got, sub);
        long callGot = 0; start = millis(); bool endMk = false;
        while (!endMk && millis() - start < 10000UL) {
          if (!otaReadLine(at, line, sizeof(line), 3000)) continue;
          char *h = strstr(line, "+HTTPREAD:");
          if (!h) { if (strstr(line, "ERROR")) { readOk = false; break; } continue; }
          long n = 0;
          if (sscanf(h, "+HTTPREAD: DATA,%ld", &n) != 1) sscanf(h, "+HTTPREAD: %ld", &n);
          if (n <= 0) { endMk = true; break; }            // "+HTTPREAD: 0" terminator
          if (n > want - got) n = want - got;             // never overrun the buffer
          if (!otaReadRaw(at, pieceBuf + got, n, 4000)) { readOk = false; break; }
          got += n; callGot += n;
          start = millis();
        }
        if (!readOk || callGot == 0) { readOk = false; break; }   // transient — re-read the piece
      }
      if (readOk && got == want) {
        if (Update.write(pieceBuf, want) != (size_t)want) { logLine("[ota] write fail p%d", piece); ok = false; break; }
        mbedtls_md_update(&md, pieceBuf, want);   // hash exactly what we flashed
        done += want; pieceOk = true;
      } else {
        logLine("[ota] retry p%d @%ld t%d", piece, got, tries); delay(800);
      }
    }
    if (!ok) break;
    if (!pieceOk) { logLine("[ota] give up p%d", piece); ok = false; break; }
    piece++;
    if (done - lastLog >= 131072) { logLine("[ota] %ld/%ld p%d", done, total, piece); lastLog = done; }
  }
  at.println("AT+HTTPTERM"); otaWaitToken(at, "OK", 3000);

  uint8_t digest[32];
  mbedtls_md_finish(&md, digest);
  mbedtls_md_free(&md);

  // Install ONLY if the image is complete AND its signature verifies against the
  // embedded public key. Signature is checked before Update.end sets the boot
  // partition, so an unsigned/tampered image never becomes bootable.
  if (ok && done >= total && otaVerifySig(at, url, digest) && Update.end(true)) {
    logLine("[ota] %s installed (%ld) - rebooting", ver, done);
    delay(300);
    ESP.restart();
  }
  int uerr = Update.getError();          // 11 = MD5 mismatch (corrupt assembly)
  Update.abort();
  logLine("[ota] failed at %ld/%ld ok=%d uerr=%d%s", done, total, ok, uerr,
          millis() - otaStart >= MAX_OTA_MS ? " (OTA time budget exhausted)" : "");
  otaNoteFailure(ver);
  modemBusy = priorBusy; mainGiveModem();
  return false;
}

static void stopDebugAp() {
  if (!debugApActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  debugApActive = false;
}

static void evaluateAlarm(Sample &s) {
  uint8_t wasActive = alarmActive;
  if (!isnan(s.tCab) && s.tCab > TEMP_ALARM_C) {
    // cap at the threshold: >= ALARM_CONSECUTIVE means latched, counting higher
    // adds nothing and would cost an NVS write-through every breaching wake
    if (consecutiveBreaches < ALARM_CONSECUTIVE) consecutiveBreaches++;
  } else {
    consecutiveBreaches = 0;
    alarmActive = 0;
  }
  if (consecutiveBreaches >= ALARM_CONSECUTIVE) alarmActive = 1;
  // Edge -> queue the alert instead of publishing here: in the double-boot
  // pattern the latch can happen on a boot that dies at modem power-on, and a
  // "compare against previous state" test on the NEXT boot then sees the alarm
  // as already-active and swallows the alert (observed live: alarm:1 in
  // telemetry, alert topic silent). The pending flag survives via NVS and is
  // published by whichever boot next has MQTT up.
  if (alarmActive && !wasActive) tempAlertPending = 1;
  s.alarm = alarmActive;
  saveMonState();                                       // no-op unless something changed
}

// A76XX firmware families disagree on the CGNSSINFO layout (3 vs 4 SV-count
// fields), so the fork's fixed-offset getGPS reads date fields as coordinates
// on this modem (seen live: lon = 130726, i.e. ddmmyy). Anchor on the N/S
// indicator instead: lat sits right before it, lon right after.
// Field order (A76XX, confirmed against the TinyGSM fork's own parser):
//   <mode>,<GPS-SVs>,<BEIDOU-SVs>,<GLONASS-SVs>,<GALILEO-SVs>,<lat>,<N/S>,<lon>,<E/W>,
//   <date>,<UTC>,<alt>,<speed>,<course>,<PDOP>,<HDOP>,<VDOP>
// Anchored on the N/S token so a firmware that drops a leading field still parses.
static bool readGnssFix(float *outLat, float *outLon, int *satsUsed, float *hdopOut, int *fixMode, int *svs) {
  if (satsUsed) *satsUsed = 0;
  if (svs) svs[0] = svs[1] = svs[2] = svs[3] = 0;
  if (hdopOut)  *hdopOut  = 0;
  if (fixMode)  *fixMode  = 0;
  String raw = atQuery("+CGNSSINFO");
  int p = raw.indexOf("+CGNSSINFO:");
  if (p < 0) return false;
  String body = raw.substring(p + 11);
  const int MAXTOK = 20;
  String tok[MAXTOK];
  int n = 0, from = 0;
  while (n < MAXTOK) {
    int c = body.indexOf(',', from);
    if (c < 0) { tok[n] = body.substring(from); tok[n].trim(); n++; break; }
    tok[n] = body.substring(from, c); tok[n].trim(); n++;
    from = c + 1;
  }
  // Visible satellites = the 4 SV-count fields after <mode> (GPS, BeiDou,
  // GLONASS, Galileo). Set even before a fix so the GNSS watchdog can tell a
  // wedged engine (0 sats) from one that's simply still acquiring (sats > 0).
  int ns = -1;
  for (int i = 1; i < n - 2; i++) {
    if ((tok[i] == "N" || tok[i] == "S") && (tok[i + 2] == "E" || tok[i + 2] == "W")) { ns = i; break; }
  }
  // Satellites in view = every field between <mode> and <lat>. The A76XX manual
  // lists four SV fields (GPS,BeiDou,GLONASS,Galileo); THIS A7608E-H firmware sends
  // three, so a fixed "four" swallowed the latitude as a satellite count (the
  // "55 GLONASS satellites" of 2.75). Bounded by the N/S anchor instead; before a
  // fix (no anchor) fall back to the first three fields.
  {
    int nSv = ns > 0 ? ns - 2 : 0;
    if (nSv == 0) { for (int i = 1; i < n && i <= 4 && tok[i].length(); i++) nSv = i; }   // pre-fix: SV fields run until the empty <lat>
    if (nSv > 4) nSv = 4;
    int sum = 0;
    for (int i = 1; i <= nSv && i < n; i++) { int v = tok[i].toInt(); sum += v; if (svs) svs[i - 1] = v; }
    if (satsUsed) *satsUsed = sum;
  }
  if (ns < 1 || !tok[ns - 1].length() || !tok[ns + 1].length()) return false;  // no fix yet
  if (fixMode) *fixMode = tok[0].toInt();                                       // 2 = 2D, 3 = 3D
  if (hdopOut && ns + 9 < n) *hdopOut = tok[ns + 9].toFloat();                  // HDOP (0 if absent)
  float lat = tok[ns - 1].toFloat();
  float lon = tok[ns + 1].toFloat();
  // Some firmware sends decimal degrees, some zero-padded ddmm.mmmmmm / dddmm.mmmmmm.
  // Decide by the token's digit count before the '.', not by magnitude: "00006.0000"
  // (0°06' E, ddmm) is 6.0 by magnitude and would have passed as 6 degrees — a 60x
  // error near the prime meridian or equator (2026-08-26 review).
  auto intDigits = [](const String &t) { int i = 0, n = 0; if (t.length() && (t[0] == '-' || t[0] == '+')) i = 1; while (i < (int)t.length() && isdigit((unsigned char)t[i])) { n++; i++; } return n; };
  if (intDigits(tok[ns - 1]) >= 4) { float d = floorf(lat / 100.0f); lat = d + (lat - d * 100.0f) / 60.0f; }   // ddmm has 4, decimal <= 2
  if (intDigits(tok[ns + 1]) >= 5) { float d = floorf(lon / 100.0f); lon = d + (lon - d * 100.0f) / 60.0f; }   // dddmm has 5, decimal <= 3
  if (tok[ns] == "S")     lat = -lat;
  if (tok[ns + 2] == "W") lon = -lon;
  if (lat == 0 && lon == 0) return false;
  *outLat = lat;
  *outLon = lon;
  return true;
}

// Called on every successful GPS fix (new position already in lastLat/lastLon,
// previous fix passed in). Anchor-based, so slow creep still trips the alert:
// parked -> anchor is home; >MOVE_ALARM_M from anchor = moving (alert once,
// fast cadence, GPS every wake); moving -> MOVE_STOP_CYCLES consecutive fixes
// within MOVE_STOP_M of each other = parked again, re-anchor at the new spot.
static void updateMovement(float prevLat, float prevLon) {
  if (anchorLat == 0 && anchorLon == 0) {               // first fix ever (NVS mirror also empty)
    anchorLat = lastLat; anchorLon = lastLon;
    saveMonState();
    return;
  }
  if (!movingActive) {
    float dAnchor = geoDistM(anchorLat, anchorLon, lastLat, lastLon);
    if (dAnchor > MOVE_ALARM_M) {
      movingActive = 1; stillStreak = 0; moveAlertPending = 1;
      logLine("[move] MOVING - %dm from anchor", (int)dAnchor);
    }
  } else {
    float dPrev = (prevLat != 0 || prevLon != 0) ? geoDistM(prevLat, prevLon, lastLat, lastLon) : 0;
    if (dPrev < MOVE_STOP_M) {
      if (++stillStreak >= MOVE_STOP_CYCLES) {
        movingActive = 0; stillStreak = 0;
        anchorLat = lastLat; anchorLon = lastLon;       // this is the new parked spot
        logLine("[move] stopped - re-anchored");
      }
    } else {
      stillStreak = 0;
    }
  }
  saveMonState();                                       // no-op unless something changed
}

// A good fix is the most expensive product of a wake, and tonight four of them
// evaporated between "obtained" and "in the DB" (PubSubClient can report a stale
// CCH session as connected, so even the 2.78 reconnect path can publish into a
// dead socket). Every good fix is therefore persisted to NVS with its epoch and
// re-published as a BACKFILLED point (its own ts, gps_backfill=1) on following
// wakes until one of the publishes actually lands; cleared only after the same
// wake ALSO delivered its normal frame, which is the best liveness signal we have.
struct PendingFix { uint32_t ts; float lat, lon, hdop; uint8_t sats; };   // one NVS blob, one write per new fix
static void savePendingFix(uint32_t ts, float lat, float lon, float hdop, uint8_t sats) {
  PendingFix f = { ts, lat, lon, hdop, sats };
  Preferences p; p.begin("freezermon", false);
  PendingFix cur = {0, 0, 0, 0, 0}; p.getBytes("pfix", &cur, sizeof(cur));
  if (memcmp(&cur, &f, sizeof(f)) != 0) p.putBytes("pfix", &f, sizeof(f));   // NVS is flash: skip identical rewrites
  p.end();
}
static void clearPendingFix() { Preferences p; p.begin("freezermon", false); if (p.isKey("pfix")) p.remove("pfix"); p.end(); }
static bool publishPendingFix() {                 // true = nothing pending or sent OK
  PendingFix f = {0, 0, 0, 0, 0};
  { Preferences p; p.begin("freezermon", true); p.getBytes("pfix", &f, sizeof(f)); p.end(); }
  uint32_t ts = f.ts; float la = f.lat, lo = f.lon, hd = f.hdop; uint8_t sa = f.sats;
  if (!ts) return true;
  if (!mqtt.connected()) return false;
  JsonDocument doc;
  // NO "device" field: the device identity is the topic-derived TAG. A field named
  // like the tag poisons every Flux pivot in the bucket ("a column named device
  // already exists") — it broke the whole dashboard the moment the first backfill
  // frame landed (21:41Z 2026-08-26; purged server-side).
  doc["ts"] = ts;
  doc["lat"] = la; doc["lon"] = lo;
  doc["hdop"] = serialized(String(hd, 1)); doc["sats"] = sa;
  doc["buffered"] = 1; doc["gps_backfill"] = 1;
  char topic[64]; snprintf(topic, sizeof(topic), "freezer/%s/telemetry", deviceId);
  String out; serializeJson(doc, out);
  bool ok = mqtt.publish(topic, out.c_str());
  pendingFixSent = ok;
  logLine("[gps] backfill fix @%lu %s", (unsigned long)ts, ok ? "sent" : "FAILED");
  return ok;
}

static bool maybeGps(uint32_t fixTimeoutS) {
  // Cadence: GPS_EVERY_N_REPORTS wakes between attempts (0 = every wake — movement
  // detection latency IS this cadence, the board has no accelerometer). While
  // moving, every wake regardless. A unit that keeps missing (parked in a garage)
  // backs off to every GPS_MISS_BACKOFF_N wakes so it doesn't burn a full window
  // every wake forever; the first success restores the configured cadence.
  uint8_t every = GPS_EVERY_N_REPORTS;
  if (gpsMissStreak >= GPS_MISS_BACKOFF_AFTER && every < GPS_MISS_BACKOFF_N) every = GPS_MISS_BACKOFF_N;
  if (!movingActive && reportsSinceGps < every) { reportsSinceGps++; return false; }
  // Retry cadence, not fix cadence: reset the counter for the ATTEMPT. A
  // no-fix cycle (unit indoors) must wait N reports again — resetting only on
  // success made GNSS hunt 90 s on EVERY wake and starve the MQTT session.
  reportsSinceGps = 0;
  // NO +CGNSSMODE traffic (2.83). Between 2.76 and 2.82 every hunt was preceded by
  // mode writes/probes ("15", "=?", "3", "3,1" — the engine kept answering 1, i.e.
  // GPS+GLONASS+Galileo, and refused the rest). In the same window the engine began
  // coming up BLIND — 0 satellites for whole windows, OUTDOORS, roughly every other
  // power cycle — which never happened before 2.76 (July: a fix every single cycle).
  // Poking the engine's mode NVRAM on every power-up is the prime suspect, and mode 1
  // was fine (11-17 sats, HDOP 1-2). This restores the exact pre-2.76 init sequence.
  if (!modem.enableGPS(GPS_ANTENNA_POWER_PIN, GPS_ANTENNA_POWER_LEVEL)) { logLine("[gps] enable failed"); return false; }
  // read-only mode query kept: it is how we learned the engine's answer, and reading
  // has never correlated with blindness the way writing might have
  { String m = atQuery("+CGNSSMODE?"); m.replace("\r", " "); m.replace("\n", " "); m.trim();
    int c = m.indexOf(':'); if (c >= 0) m = m.substring(c + 1); m.trim(); int sp = m.indexOf(' '); if (sp > 0) m = m.substring(0, sp);
    strlcpy(gnssModeStr, m.c_str(), sizeof(gnssModeStr)); }
  // Assisted GNSS: pull current ephemeris from SIMCom's AGNSS server over the (already
  // attached) data bearer so a cold start fixes in seconds instead of timing out at 90 s.
  // AGPS data stays valid a few hours → refresh at most every ~2 h to spare the SIM.
  // Best-effort: on failure we fall back to an unassisted cold fix, so it can only help.
  // AGPS validity tracks MODEM POWER CYCLES, not wall-clock time: +CAGPS loads
  // ephemeris into the GNSS engine's RAM, which dies with the modem rail — on
  // battery that's every deep sleep. The NVS-persisted timestamp alone (2.64)
  // skipped the re-download and left the engine cold-starting unassisted in a
  // 30 s window: zero fixes from a spot that fixed every wake the day before.
  // agpsLoaded is cleared by modemPowerOn(); the 2 h wall-clock cap only
  // matters in the powered regime, where the modem stays up between cycles.
  bool agpsFresh = agpsLoaded && rtcEpoch && lastAgpsEpoch && (rtcEpoch - lastAgpsEpoch) < AGPS_REFRESH_S;
  if (!agpsFresh) {
    modem.sendAT("+CAGPS");
    if (modem.waitResponse(20000L) == 1) { agpsLoaded = true; lastAgpsEpoch = rtcEpoch; saveMonState(); logLine("[gps] AGPS ephemeris loaded"); }
    else                                   logLine("[gps] AGPS load failed - unassisted cold fix");
  }
  // Quality gate (2.70): the first sentence with a lat/lon is typically a 4-5 SV,
  // HDOP 3-5 fix — a +/-100 m position. Today's bench data: 40 first-fixes spread
  // 196 m N-S, enough to trip MOVE_ALARM_M from an unlucky anchor. So a fix must
  // be 3D (GPS_REQUIRE_3D) with HDOP <= GPS_MAX_HDOP, and after the first good one
  // we keep polling for GPS_SETTLE_S and publish the LOWEST-HDOP sample. A window
  // that never produces a good fix reports no coordinate at all — absence is the
  // signal, a bad coordinate is not.
  float lat, lon, hdop;
  int sats = 0, maxSats = 0, mode = 0;
  bool gotFix = false, rejectLogged = false;
  float bestHdop = 1e9f, bestLat = 0, bestLon = 0, bestAnyHdop = 0;
  bool anyFix = false;
  int svs[4] = {0, 0, 0, 0}, maxSvs[4] = {0, 0, 0, 0};
  uint32_t firstGoodMs = 0;
  float prevLat = lastLat, prevLon = lastLon;           // previous fix, for movement detection
  uint32_t start = millis();
  uint32_t windowMs = fixTimeoutS * 1000UL;
  while (millis() - start < windowMs && awakeBudgetLeft()) {
    bool fix = readGnssFix(&lat, &lon, &sats, &hdop, &mode, svs);
    if (sats > maxSats) { maxSats = sats; memcpy(maxSvs, svs, sizeof(maxSvs)); }
    if (maxSats == 0 && millis() - start >= (uint32_t)GPS_ZERO_SAT_ABORT_S * 1000UL) {
      logLine("[gps] 0 satellites after %ds - engine wedged or no antenna, aborting hunt", GPS_ZERO_SAT_ABORT_S);
      break;                                            // 20:47/20:50Z 2026-08-26: two 0-sat windows of 33 s and 91 s
    }
    if (fix) {
      anyFix = true;
      if (hdop > 0 && (bestAnyHdop == 0 || hdop < bestAnyHdop)) bestAnyHdop = hdop;
      bool good = (mode >= 3 || !GPS_REQUIRE_3D) && hdop > 0 && hdop <= GPS_MAX_HDOP;
      // A fix that exists but fails the gate is converging: give the short cold-boot
      // window the full periodic window rather than throwing the fix away at 30 s.
      if (!good && windowMs < (uint32_t)GPS_FIX_TIMEOUT_S * 1000UL) {
        windowMs = (uint32_t)GPS_FIX_TIMEOUT_S * 1000UL;
        logLine("[gps] fix present but hdop=%.1f mode=%d - extending window to %ds", hdop, mode, GPS_FIX_TIMEOUT_S);
      }
      if (good) {
        if (hdop < bestHdop) { bestHdop = hdop; bestLat = lat; bestLon = lon; }
        if (!firstGoodMs) firstGoodMs = millis();
        if (millis() - firstGoodMs >= (uint32_t)GPS_SETTLE_S * 1000UL || hdop <= 1.0f) break;   // settled
      } else if (!rejectLogged) {
        logLine("[gps] fix rejected mode=%d hdop=%.1f sats=%d - waiting for a better one", mode, hdop, sats);
        rejectLogged = true;
      }
    }
    delay(2000);
  }
  if (firstGoodMs) gpsMissStreak = 0; else if (gpsMissStreak < 255) gpsMissStreak++;
  gpsLastStatus = firstGoodMs ? 3 : anyFix ? 2 : 1;
  gpsLastSats   = (uint8_t)(maxSats > 255 ? 255 : maxSats);
  snprintf(gpsLastSvs, sizeof(gpsLastSvs), "%d/%d/%d/%d", maxSvs[0], maxSvs[1], maxSvs[2], maxSvs[3]);
  gpsLastSecs   = (uint16_t)((millis() - start) / 1000UL);
  gpsLastHdop   = firstGoodMs ? bestHdop : bestAnyHdop;
  if (firstGoodMs) {
    lastLat = bestLat; lastLon = bestLon;
    lastFixHdop = bestHdop; lastFixSats = maxSats;
    if (rtcEpoch) savePendingFix(rtcEpoch, bestLat, bestLon, bestHdop, (uint8_t)(maxSats > 255 ? 255 : maxSats));
    gotFix = true;
    logLine("[gps] fix hdop=%.1f sats=%d after %lus (settle %lus)", bestHdop, maxSats,
            (firstGoodMs - start) / 1000UL, (millis() - firstGoodMs) / 1000UL);
  }
  if (gotFix) { gpsFreshThisWake = true; updateMovement(prevLat, prevLon); }
  // GNSS-wedge watchdog: an engine that is powered but tracks 0 satellites the
  // whole window is stuck (only a supply cut recovers it). Seeing any sat —
  // even without a full fix — means it's alive, just acquiring.
  if (maxSats > 0) gnssZeroSatStreak = 0;
  else if (gnssZeroSatStreak < 255) gnssZeroSatStreak++;
  if (!gotFix) logLine("[gps] no fix (maxSats=%d %s, wedge streak=%u)", maxSats, gpsLastSvs, gnssZeroSatStreak);
  modem.disableGPS(GPS_ANTENNA_POWER_PIN, 0);           // GNSS + antenna rail off before publish/sleep
  // GNSS shares the RF path and spews URCs; give LTE a moment to settle and
  // resync the AT parser, then re-verify the data bearer before MQTT.
  delay(1500);
  modem.sendAT("");                                     // ping — realigns waitResponse framing
  modem.waitResponse(2000);
  if (!modem.isGprsConnected()) {
    logLine("[gps] PDP dropped during GNSS - reattaching");
    modem.gprsConnect(APN, GPRS_USER, GPRS_PASS);
  }
  return gotFix;
}

// Li-ion must not be charged below ~0 C (lithium plating -> permanent damage).
// The board's CN3065 charger HAS a TEMP protection input but it is wired to GND
// (disabled) on the T-A7608, so detect the condition instead: external power
// present (the charger has input) while the battery probe — the second DS18B20,
// t_amb, strapped to the cell — reads below COLD_CHARGE_C. Returns true exactly
// once per episode (edge-triggered); clears with +2 C hysteresis so it re-arms.
// No probe fitted (t_amb NAN) -> never fires.
static bool coldChargeCheck(const Sample &s) {
  if (isnan(s.tAmb)) return false;
  if (s.extPower && s.tAmb < COLD_CHARGE_C) {
    if (!coldChargeActive) {
      coldChargeActive = 1;
      logLine("[batt] COLD CHARGE - %.1fC on external power, unplug or warm the cell", s.tAmb);
      return true;
    }
  } else if (!s.extPower || s.tAmb > COLD_CHARGE_C + 2.0f) {
    coldChargeActive = 0;
  }
  return false;
}

static bool publishSample(const Sample &s, uint32_t nowEpoch, bool buffered,
                          const char *wakeReason, int16_t rssiDbm) {
  JsonDocument doc;
  uint32_t ts = nowEpoch > s.ageS ? nowEpoch - s.ageS : 0;
  if (ts) doc["ts"] = ts;               // omitted only if clock never synced since first power-on
  if (!isnan(s.tCab)) doc["t_cab"] = serialized(String(s.tCab, 2));
  if (!isnan(s.tAmb)) doc["t_amb"] = serialized(String(s.tAmb, 2));
  doc["door"]      = s.doorOpen;
  doc["vbat_mv"]   = s.vbatMv;
  doc["vsolar_mv"] = s.vsolarMv;
  if (rssiDbm > -900) doc["rssi_dbm"] = rssiDbm;
  doc["ext_power"] = s.extPower;
  // Coords are published ONLY when a fix was obtained this wake — a stale
  // coordinate would mask "cannot get a fix", and absence of data IS the
  // signal (Peter's rule). Last-known position lives in the DB history, not
  // fabricated into current frames. Range guard: a misparse must never map
  // the unit to the Pacific.
  if (gpsFreshThisWake &&
      (lastLat != 0 || lastLon != 0) && fabsf(lastLat) <= 90.0f && fabsf(lastLon) <= 180.0f) {
    doc["lat"] = lastLat;
    doc["lon"] = lastLon;
    doc["hdop"] = serialized(String(lastFixHdop, 1));
    doc["sats"] = lastFixSats;
  }
  if (g_resetStr && strcmp(g_resetStr, "deepsleep") != 0) {   // reset forensics on every non-sleep boot
    doc["rr0"] = (int)rtc_get_reset_reason(0);            // ROM-level per-core reset reasons (rom/rtc.h)
    doc["rr1"] = (int)rtc_get_reset_reason(1);
    if (crashSnapshot.length()) {                          // published once, budgeted to what the frame can still carry
      size_t used = measureJson(doc) + 24;                                   // 24 = key + quotes + margin
      size_t room = used < PAYLOAD_MAX - 1 ? PAYLOAD_MAX - 1 - used : 0;
      String cl = crashSnapshot; if (cl.length() > room) cl = cl.substring(0, room > 0 ? room : 0);
      if (cl.length()) doc["crash_log"] = cl;
      crashSnapshot = ""; crashMagic = 0;
    }
  }
  if (gpsLastStatus) {                                   // outcome of the most recent hunt (see gpsLastStatus)
    doc["gps_last"]      = gpsLastStatus;
    doc["gps_last_s"]    = gpsLastSecs;
    doc["gps_last_sats"] = gpsLastSats;                  // satellites in view (max over the window), fix or not
    doc["gps_last_svs"]  = gpsLastSvs;                   // per-constellation split, sentence order (see readGnssFix)
    if (gnssModeStr[0]) doc["gnss_mode"] = gnssModeStr;  // what the engine accepted from +CGNSSMODE=15
    if (gnssModesStr[0]) doc["gnss_modes"] = gnssModesStr; // what it says it supports (+CGNSSMODE=?)
    if (gpsLastHdop > 0) doc["gps_last_hdop"] = serialized(String(gpsLastHdop, 1));
  }
  doc["alarm"]     = s.alarm;
  doc["moving"]    = movingActive;
  doc["boot"]      = bootCount;
  doc["buffered"]  = buffered ? 1 : 0;
  doc["wake"]      = wakeReason;
  doc["rst"]       = g_resetStr;      // reset reason (diagnosing the no-sleep loop)
  doc["ph"]        = prevPhase;       // how far the PREVIOUS cycle got (NVS breadcrumb, 5=reached sleep entry)
  doc["bo_streak"] = brownoutStreak;  // consecutive brownout boots (0 on a clean wake); >= BROWNOUT_SHED_AFTER sheds GNSS
  if (sagRestMv) {                    // supply sag through modem power-on (this wake, survived): rest, min, when, which step
    doc["vbat_rest_mv"] = sagRestMv;
    doc["vbat_min_mv"]  = sagMinMv;
    doc["vbat_sag_ms"]  = sagAtMs;
    doc["vbat_sag_at"]  = sagStep;    // 1 tap-precharge 2 rail-on 3 reset-pulse 4 PWRKEY 5 modem-boot
    doc["vbat_sag_n"]   = sagDeepN;   // samples >300 mV below rest (needle vs sustained sag)
  }
  doc["fw"]        = FW_VERSION;      // fleet version tracking + OTA confirmation

  // 768: serializeJson TRUNCATES SILENTLY at the buffer size and returns bytes
  // written — with the 2.7x diagnostic fields a coordinate-carrying frame passed
  // 384 bytes and every one of them reached Telegraf as invalid JSON ("unable to
  // parse ... bo_streak:"), which is where ALL of tonight's "lost fixes" actually
  // died. The dead-socket theories (2.78/2.82/2.84) were chasing this. The guard
  // below makes the failure loud if the frame ever outgrows the buffer again.
  char topic[64], payload[PAYLOAD_MAX];         // largest frame (all breadcrumbs + budgeted crash_log) is ~820 B; keep headroom
  snprintf(topic, sizeof(topic), "freezer/%s/telemetry", deviceId);
  size_t n = serializeJson(doc, payload, sizeof(payload));
  size_t need = measureJson(doc);
  if (n < need) {
    logLine("[mqtt] frame TRUNCATED %u/%u bytes - NOT publishing garbage; grow payload[]", (unsigned)n, (unsigned)need);
    return false;                                        // a clipped frame is invalid JSON: worse than no frame
  }
  // live sample retained (last-known state on broker); backfill not retained
  return mqtt.publish(topic, (const uint8_t *)payload, n, !buffered);
}

static void publishAlert(const Sample &s, uint32_t nowEpoch, const char *kind) {
  JsonDocument doc;
  doc["ts"] = nowEpoch;
  doc["kind"] = kind;                                   // temp_breach | door_open | batt_low
  if (!isnan(s.tCab)) doc["t_cab"] = serialized(String(s.tCab, 2));
  doc["vbat_mv"] = s.vbatMv;
  char topic[64], payload[256];
  snprintf(topic, sizeof(topic), "freezer/%s/alert", deviceId);
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, (const uint8_t *)payload, n, false);
}

static void bufferSample(const Sample &s) {
  if (rtcBufCount >= BUF_MAX) {                         // drop oldest, keep newest
    memmove(&rtcBuf[0], &rtcBuf[1], sizeof(Sample) * (BUF_MAX - 1));
    rtcBufCount = BUF_MAX - 1;
  }
  rtcBuf[rtcBufCount++] = s;
}

static void flushBuffer(uint32_t nowEpoch, int16_t rssiDbm) {
  // cursor-based: drop only confirmed-sent entries so a mid-flush failure
  // doesn't resend already-delivered samples next cycle
  uint8_t sent = 0;
  for (; sent < rtcBufCount && awakeBudgetLeft(); sent++) {
    if (!publishSample(rtcBuf[sent], nowEpoch, true, "backfill", rssiDbm)) break;
    delay(50);
  }
  if (sent && sent < rtcBufCount) {
    memmove(&rtcBuf[0], &rtcBuf[sent], sizeof(Sample) * (rtcBufCount - sent));
  }
  rtcBufCount -= sent;
}

static void goToSleep(uint32_t seconds) {
  stopDebugAp();                                        // WiFi off before sleep
  if (mqtt.connected()) mqtt.disconnect();
  if (modem.isGprsConnected()) modem.gprsDisconnect();
  modem.poweroff();                                     // modem fully off while sleeping
  delay(200);
  markPhase(5);                                         // teardown done (wifi+mqtt+modem off)

  // Age the epoch estimate by this wake's awake time now; the SLEEP part is measured
  // at the next boot from the RTC clock (which keeps running in deep sleep) — a
  // door-open wake ends the sleep early, and aging by the scheduled length put
  // backfilled timestamps minutes to hours in the future (2026-08-26 review).
  if (rtcEpoch) rtcEpoch += (millis() - awakeStart) / 1000UL;
  { struct timeval tv; gettimeofday(&tv, NULL); sleepStartUs = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec; }

  // door-open wake: reed opens -> pin pulled HIGH. RTC-domain pullup required —
  // the digital-domain INPUT_PULLUP dies in deep sleep.
  rtc_gpio_init(DOOR_PIN);
  rtc_gpio_set_direction(DOOR_PIN, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(DOOR_PIN);
  rtc_gpio_pulldown_dis(DOOR_PIN);
  // only arm door-wake while the door is closed — a propped-open door would
  // re-trigger EXT1 instantly, causing back-to-back LTE sessions; the fast
  // timer interval covers reporting while it stays open
  if (rtc_gpio_get_level(DOOR_PIN) == 0) {
    esp_sleep_enable_ext1_wakeup(1ULL << DOOR_PIN, ESP_EXT1_WAKEUP_ANY_HIGH);
  }

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  logLine("[sleep] %lus, buffered=%u", seconds, rtcBufCount);
  SerialMon.flush();
  markPhase(6);                                         // next instruction is deep sleep itself
  esp_deep_sleep_start();
}

// ---------- main ----------
void setup() {
  awakeStart = millis();
  bootCount++;
  if (sleepStartUs) {                                   // woke from deep sleep: age RTC state by the MEASURED sleep
    struct timeval tv; gettimeofday(&tv, NULL);
    int64_t nowUs = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
    uint32_t slept = nowUs > sleepStartUs ? (uint32_t)((nowUs - sleepStartUs) / 1000000LL) : 0;
    for (uint8_t i = 0; i < rtcBufCount; i++) rtcBuf[i].ageS += slept;
    if (rtcEpoch) rtcEpoch += slept;
    sleepStartUs = 0;
  }
  SerialMon.begin(115200);
  {                      // crash forensics: snapshot the pre-reset log tail before anything logs
    esp_reset_reason_t r0 = esp_reset_reason();
    if ((r0 == ESP_RST_INT_WDT || r0 == ESP_RST_TASK_WDT || r0 == ESP_RST_PANIC || r0 == ESP_RST_WDT) &&
        crashMagic == 0xC0DEC0DEUL) {
      for (uint8_t i = 0; i < CRASH_LINES; i++) {
        const char *l = crashRing[(crashHead + i) % CRASH_LINES];
        if (!l[0]) continue;
        if (crashSnapshot.length()) crashSnapshot += " | ";
        crashSnapshot += l;
      }
    }
  }
  resolveDeviceId();     // NVS -> deviceId, before the AP SSID / MQTT topics use it
  if (!modemLock) modemLock = xSemaphoreCreateRecursiveMutex();   // before the dbgweb task can exist
  { Preferences p; p.begin("freezermon", false);                  // a successful install is the only thing that clears OTA failure memory
    if (p.getString("otafw", "") != FW_VERSION) { p.putString("otafw", FW_VERSION); p.putUChar("otatc", 0); p.putUChar("otafc", 0); }
    p.end(); }

  // silicon-enforced sleep guarantee: if any modem call wedges past the awake
  // budget, the task WDT resets the chip instead of draining the battery
  esp_task_wdt_init(MAX_AWAKE_MS / 1000 + 60, true);
  esp_task_wdt_add(NULL);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool doorWake = cause == ESP_SLEEP_WAKEUP_EXT1;
  bool coldBoot = !doorWake && cause != ESP_SLEEP_WAKEUP_TIMER;
  const char *wakeReason = doorWake ? "door" : coldBoot ? "power_on" : "timer";
  // Reset reason distinguishes a real cold boot from a fault loop: BROWNOUT =
  // the modem's current burst is sagging the supply (weak/cold battery or a
  // <2 A source); PANIC = firmware crash; TASK_WDT/INT_WDT = something wedged.
  esp_reset_reason_t rr = esp_reset_reason();
  const char *resetStr =
      rr == ESP_RST_POWERON   ? "poweron"   : rr == ESP_RST_BROWNOUT ? "BROWNOUT" :
      rr == ESP_RST_PANIC     ? "PANIC"     : rr == ESP_RST_TASK_WDT ? "TASK_WDT" :
      rr == ESP_RST_INT_WDT   ? "INT_WDT"   : rr == ESP_RST_DEEPSLEEP ? "deepsleep" :
      rr == ESP_RST_SW        ? "sw"        : "other";
  g_resetStr = resetStr;
  { Preferences p; p.begin("freezermon", true); prevPhase = p.getUChar("ph", 0); brownoutStreak = p.getUChar("bos", 0); p.end(); }
  // Count brownout BOOTS; reset only once a wake has actually published (markPhase 3),
  // not merely on waking from deep sleep. In the double-boot loop every timer wake
  // dies at the inrush (no frame) and only the brownout reboot completes, so a
  // reset-on-wake made the streak alternate 0/1 forever and the GNSS shed never
  // engaged (20:43-20:45Z 2026-08-26).
  if (rr == ESP_RST_BROWNOUT) { if (brownoutStreak < 255) brownoutStreak++; Preferences p; p.begin("freezermon", false); p.putUChar("bos", brownoutStreak); p.end(); }
  markPhase(1);                                          // cycle started
  // rr0/rr1 = ROM-level per-core reset reasons (rom/rtc.h) — e.g. 12=SW_CPU,
  // 14=EXT_CPU, 15=BROWNOUT, 5=DEEPSLEEP; distinguishes esp_restart() from a
  // panic-reboot whose PANIC hint (stored in RTC, wiped) degraded to plain "sw".
  logLine("[boot] #%lu fw=%s id=%s wake=%s reset=%s rr0=%d rr1=%d prev_ph=%u",
          bootCount, FW_VERSION, deviceId, wakeReason, resetStr,
          (int)rtc_get_reset_reason(0), (int)rtc_get_reset_reason(1), prevPhase);
  // RTC survived only if we truly woke from deep sleep; on every other reset
  // (brownout, sw, power-on) restore the monitor state from its NVS mirror so
  // the temp-alarm streak, movement anchor and AGPS age survive the wipe.
  if (rr != ESP_RST_DEEPSLEEP) loadMonState();

  // cold boot = installer likely present -> bring up the field-debug AP.
  // EXCEPT after a brownout: the AP's WiFi load stacks onto the already-heavy
  // cold-boot cycle (AGPS + GPS hunt + LTE attach + double publish) and can
  // re-trigger the very sag that caused the reset — a self-sustaining reboot
  // loop (seen live 2026-07-14: boot:1/power_on every cycle for an hour).
  // Shedding the AP makes the recovery cycle lighter so the loop can break;
  // a healthy cold boot (power-on/reset button) still gets the console.
  bool crashReset = rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_PANIC;
  if (coldBoot && rr != ESP_RST_BROWNOUT && !crashReset) startDebugAp();
  else if (coldBoot) logLine("[boot] debug AP skipped - %s recovery cycle", crashReset ? "crash" : "brownout");

  Sample s;
  readSensors(s);
  evaluateAlarm(s);
  lastSample = s; lastSampleValid = true;               // /status reads this copy

  bool published = false;
  if (awakeBudgetLeft()) {
    mainTakeModem();                                    // console handlers answer "busy" until released
    modemBusy = true;                                   // /update's flash-claim check reads this
    modemPowerOn();
    if (modemConnect()) {
      markPhase(2);                                     // attached — later death = RF-load, not inrush
      int csq = modem.getSignalQuality();
      int16_t rssiDbm = (csq >= 0 && csq < 99) ? (int16_t)(-113 + 2 * csq) : -999;  // 99 = unknown
      // GNSS is deferred until AFTER publish: a 90 s no-fix hunt both eats the
      // connect budget and leaves the modem's CCH/SSL stack wedged, so the
      // MQTT connect that follows fails rc=-2 even by IP. Connect first on the
      // clean freshly-attached modem (the state that worked live at 11:12),
      // then do GPS — its fix feeds the NEXT cycle's frame via lastLat/lastLon.
      uint32_t now = networkEpoch();
      if (now) rtcEpoch = now;          // sync the RTC estimate
      else     now = rtcEpoch;          // clock query failed -> use aged estimate

      mqtt.setBufferSize(1400);      // >= topic + headers + payload[1152]   // must exceed topic + largest frame (payload[768])
      mqtt.setSocketTimeout(15);
      mqtt.setKeepAlive(180);      // OTA downloads run minutes with no mqtt.loop() — don't let the broker drop us mid-flash
      mqtt.setCallback(mqttCallback);
      char clientId[48];
      snprintf(clientId, sizeof(clientId), "freezermon-%s", deviceId);
      // Retry the connect — the A76XX TLS/CCH stack occasionally drops a first
      // CCHOPEN. Use the hostname (matches the cert SAN, valid SNI, resolved
      // via the network-provided DNS); the IP is only a last-resort fallback,
      // and it connects solely because the modem runs authmode=0 (no cert check).
      const char *hosts[] = {
        MQTT_HOST,
        MQTT_HOST,
#ifdef MQTT_HOST_IP
        MQTT_HOST_IP,
#else
        MQTT_HOST,
#endif
      };
      const int nHosts = sizeof(hosts) / sizeof(hosts[0]);
      bool mqttUp = false;
      for (int attempt = 0; attempt < nHosts && awakeBudgetLeft(); attempt++) {
        mqtt.setServer(hosts[attempt], MQTT_PORT);
        if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) { mqttUp = true; break; }
        logLine("[mqtt] attempt %d to %s failed rc=%d csq=%d", attempt + 1, hosts[attempt], mqtt.state(), modem.getSignalQuality());
        netClient.stop();                               // close the half-open TLS/CCH session
        delay(2000);
        esp_task_wdt_reset();
      }
      if (mqttUp) {
        flushBuffer(now, rssiDbm);                      // backfill offline gap first
        publishPendingFix();                            // a fix a previous wake could not deliver
        published = publishSample(s, now, false, wakeReason, rssiDbm);

        if (tempAlertPending) { publishAlert(s, now, "temp_breach"); tempAlertPending = 0; saveMonState(); }
        if (doorWake && s.doorOpen)     publishAlert(s, now, "door_open");
        if (s.vbatMv > 0 && s.vbatMv < BATT_LOW_MV) publishAlert(s, now, "batt_low");
        if (coldChargeCheck(s))         publishAlert(s, now, "cold_charge");

        // OTA check: the retained cmd (if any) arrives within a moment of subscribing
        char cmdTopic[64];
        snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/cmd", deviceId);
        mqtt.subscribe(cmdTopic);
        { char echoTopic[64]; snprintf(echoTopic, sizeof(echoTopic), "freezer/%s/telemetry", deviceId); mqtt.subscribe(echoTopic); }
        uint32_t tCmd = millis();
        while (millis() - tCmd < 3000UL && (!otaPending || !mqttEchoSeen)) { mqtt.loop(); delay(20); }
        if (otaPending) {
          if (s.extPower || s.vbatMv >= OTA_MIN_VBAT_MV) {
            performOta(otaUrl, otaVer);       // reboots on success
          } else {
            logLine("[ota] deferred - battery %umV below %d", s.vbatMv, OTA_MIN_VBAT_MV);
          }
          otaPending = false;
        }
        // Delivery-verified clear: this wake's backfill went out AND the broker echoed
        // back the retained frame published right after it on the same socket. A dead
        // socket fails the echo; an absent retained cmd no longer matters.
        if (pendingFixSent && mqttEchoSeen) clearPendingFix();
      } else {
        // Attach + IP can succeed while the carrier drops the user plane
        // (the 2026-07-13 outage — a dead SIM). Log PDP state so a repeat is
        // recognisable; the /lte console is the on-demand connectivity probe.
        logLine("[mqtt] all attempts failed rc=%d pdp=%s", mqtt.state(), atQuery("+CGACT?").c_str());
      }
      // GNSS last, on the now-idle modem — it can wedge the CCH/SSL stack, but
      // MQTT is already done for this cycle and the modem is powered off before
      // sleep, so the next wake starts clean. Skips itself on external power
      // (loop() owns GNSS there) to avoid dropping a held MQTT session.
      // GNSS: on a cold boot use a shorter, bounded window (AGPS makes fixes
      // fast) instead of the full 90 s, and — if a fix lands AND the session
      // survived GNSS — send a follow-up frame carrying the coords THIS wake
      // Same ts, so InfluxDB upserts the coords onto this cycle's point. Periodic
      // timer wakes keep the full 90 s window and send the same follow-up frame
      // on a fix (2.68) — the pre-2.66 "carry lastLat/lastLon next wake" path
      // no longer exists, coords are never replayed.
      //
      // EXCEPT on brownout recovery: forensics (fw 2.61, rst/ph telemetry)
      // proved the reboot loop lives HERE — a brownout wipes RTC, which forces
      // GPS+AGPS on the next boot, whose GNSS+RF load browns the cell out
      // again (every cycle died at ph=1, mid-GNSS, after a clean publish).
      // Shed the GNSS load for the recovery cycle so it reaches deep sleep;
      // the GPS cadence counter is set so the next attempt waits a normal
      // interval. One marginal GPS cycle then costs one light recovery cycle
      // instead of looping forever.
      // Shed GNSS only when the PREVIOUS cycle died under RF load (breadcrumb
      // ph 2-5: attached but never finished). A brownout with prev_ph==1 or 6
      // is the wake-time power-on inrush (double-boot pattern) — the attach
      // just proved the cell carries the RF load fine, so GPS stays enabled;
      // shedding it there suppressed GPS permanently (the 2.62 regression).
      if (!s.extPower) {
        bool gotFix = false;
        if (rr == ESP_RST_BROWNOUT && prevPhase >= 2 && prevPhase <= 5) {
          reportsSinceGps = 0;                          // retry GPS only after the normal cadence
          logLine("[gps] skipped - prev cycle died under RF load (ph=%u)", prevPhase);
        } else if (rr == ESP_RST_BROWNOUT && brownoutStreak >= BROWNOUT_SHED_AFTER) {
          reportsSinceGps = 0;
          logLine("[gps] skipped - %u consecutive brownout boots, light recovery cycle", brownoutStreak);
        } else {
          gotFix = maybeGps(coldBoot ? GPS_FIRST_BOOT_TIMEOUT_S : GPS_FIX_TIMEOUT_S);
        }
        // A fix lands AFTER this wake's frame went out, and since 2.66 coords are
        // published only when fresh THIS wake (gpsFreshThisWake) — so the
        // follow-up frame is the ONLY way a fix reaches the DB. Gating it on
        // coldBoot (2.66-2.67) silently dropped every timer-wake fix; it went
        // unnoticed because the July verification unit was brownout-looping and
        // every cycle was a cold boot. Any wake with a fix sends the follow-up.
        // `now` guard: if the clock never synced this cycle, `ts` is omitted from
        // both frames and the same-ts upsert can't happen -> skip to avoid a
        // duplicate point (the fix is retried after the normal GPS cadence).
        if (gotFix && published && now) {
          // GNSS shares the RF path and can drop the CCH session underneath MQTT;
          // 20:22Z 2026-08-26: gps_last=3 after a 30 s hunt, no coordinates ever
          // published. A fix is the most expensive thing this wake produced — spend
          // a few seconds on a fresh MQTT connect rather than throw it away.
          if (!mqtt.connected()) {
            logLine("[gps] mqtt dropped during GNSS - reconnecting for the fix frame");
            netClient.stop();
            char cid[48]; snprintf(cid, sizeof(cid), "freezermon-%s", deviceId);
            mqtt.setServer(MQTT_HOST, MQTT_PORT);
            if (!mqtt.connect(cid, MQTT_USER, MQTT_PASS)) { delay(1500); mqtt.connect(cid, MQTT_USER, MQTT_PASS); }
          }
          if (mqtt.connected()) {
            // publish() returning true proves nothing — 2.82 cleared the pending fix on
            // it and four more fixes vanished into dead sockets (21:25/21:29Z). The
            // pending copy survives until a LATER wake both sends it AND sees inbound
            // MQTT traffic (the retained cmd), the only two-way liveness we have.
            publishSample(s, now, false, wakeReason, rssiDbm);
          } else { gpsLastStatus = 4; logLine("[gps] fix deferred - mqtt down after GNSS (rc=%d), backfills next wake", mqtt.state()); }
        }
        // movement transition detected by this (or an earlier, offline) fix —
        // fast cadence + GPS-every-wake are already active via movingActive
        if (moveAlertPending && mqtt.connected()) {
          publishAlert(s, now, "moving");
          moveAlertPending = 0;
          saveMonState();
        }
      }
    } else {
      logLine("[net] no connectivity this cycle");
    }
    modemBusy = false;                                  // console may query the modem again
    mainGiveModem();
  }
  markPhase(3);                                         // modem work done
  if (rr != ESP_RST_BROWNOUT && published && brownoutStreak) {   // a non-brownout wake got its frame out: loop broken
    brownoutStreak = 0; Preferences p; p.begin("freezermon", false); p.putUChar("bos", 0); p.end();
  }

  if (!published) bufferSample(s);                      // data outlives connectivity

  // Deep sleep is a battery measure. On external power there is nothing to
  // protect: stay awake with the session up, poll the door continuously
  // (instant alerts instead of wake latency) and report on the powered cadence.
  if (s.extPower && published && mqtt.connected()) {
    poweredSession = true;
    mainTakeModem();                                    // loop() owns the UART for the rest of this boot
    lastReportMs = millis();                            // liveness baseline for loop()
    lastDoor = s.doorOpen;
    prevAlarm = s.alarm;
    lastReportMs = millis();
    if (!debugApActive && rr != ESP_RST_INT_WDT && rr != ESP_RST_TASK_WDT && rr != ESP_RST_PANIC)
      startDebugAp();                                   // console always on while powered — except right after a crash (loop breaker)
    logLine("[power] external power present - staying awake");
    return;                                             // continues in loop()
  }

  // on battery after a cold boot, hold the debug console open before the
  // first deep sleep so an installer can inspect status/logs
  if (debugApActive) {
    logLine("[debug] console open %ds, then sleeping", DEBUG_AP_WINDOW_S);
    uint32_t endMs = millis() + (uint32_t)DEBUG_AP_WINDOW_S * 1000UL;
    while (millis() < endMs || Update.isRunning()) {   // never sleep mid-OTA
      esp_task_wdt_reset();                            // console served by dbgweb task
#ifdef OTA_MANIFEST_URL
      if (otaCheckRequested) {                         // /update "check online" pressed
        checkOnlineUpdate();
        if (otaPending) {
          if (s.extPower || s.vbatMv >= OTA_MIN_VBAT_MV) {
            performOta(otaUrl, otaVer);                // reboots on success
          } else {
            logLine("[ota] deferred - battery %umV below %d", s.vbatMv, OTA_MIN_VBAT_MV);
          }
          otaPending = false;
        }
      }
#endif
      delay(50);
    }
  }
  markPhase(4);                                         // console window done, heading to sleep

  uint32_t interval = (s.alarm || s.doorOpen || movingActive) ? REPORT_INTERVAL_FAST_S
                                                              : REPORT_INTERVAL_S;
  goToSleep(interval);
}

// Runs only in powered mode; battery cycles never reach it (deep sleep ends them).
static uint32_t updateRunningSinceMs = 0;
void loop() {
  if (!poweredSession) goToSleep(REPORT_INTERVAL_S);    // safety net

  esp_task_wdt_reset();
  mqtt.loop();
  if (Update.isRunning()) {                             // OTA in progress — don't report or sleep
    // ... but never forever: a /update upload whose client vanished leaves Update
    // "running" with no completion handler, and this early return then starves
    // reporting indefinitely while feeding the watchdog (2026-08-27 10:05Z: first
    // powered session, silent for an hour). Abort after 5 min without completion.
    if (!updateRunningSinceMs) updateRunningSinceMs = millis();
    if (millis() - updateRunningSinceMs > 300000UL) {
      logLine("[ota] Update stuck running 5 min - aborting");
      Update.abort(); otaUploadActive = false; upBinDone = false; upSigLen = 0; upFwBlocked = false; updateRunningSinceMs = 0;
    } else { delay(5); return; }
  } else updateRunningSinceMs = 0;
  // Powered-regime liveness: if nothing has been published for 3 intervals, whatever
  // the cause, drop to a fresh cycle instead of idling on mains with no telemetry.
  if (millis() - lastReportMs > 3UL * REPORT_INTERVAL_POWERED_S * 1000UL + 60000UL) {
    logLine("[power] no report for %lus - forcing a fresh cycle", (millis() - lastReportMs) / 1000UL);
    goToSleep(REPORT_INTERVAL_FAST_S);
  }
#ifdef OTA_MANIFEST_URL
  if (otaCheckRequested) {
    checkOnlineUpdate();                                // own client on mux 1 — the MQTT session normally survives
    if (!otaPending && !mqtt.connected()) {             // restore only if it actually dropped
      char clientId[48];
      snprintf(clientId, sizeof(clientId), "freezermon-%s", deviceId);
      if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
        char cmdTopic[64];
        snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/cmd", deviceId);
        mqtt.subscribe(cmdTopic);
        snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/telemetry", deviceId);
        mqtt.subscribe(cmdTopic);                       // echo subscription (delivery proof)
      } else {
        goToSleep(REPORT_INTERVAL_FAST_S);              // recover via a fresh cycle
      }
    }
  }
#endif
  if (otaPending) {                                     // powered = always allowed
    performOta(otaUrl, otaVer);                         // reboots on success
    otaPending = false;
    if (!mqtt.connected()) goToSleep(REPORT_INTERVAL_FAST_S);  // failed OTA dropped the link
  }
  delay(250);

  pinMode(DOOR_PIN, INPUT_PULLUP);
  uint8_t door = digitalRead(DOOR_PIN) == HIGH ? 1 : 0;
  bool doorChanged = door != lastDoor;
  // while moving, report at the fast cadence even on external power (live tracking)
  uint32_t poweredInterval = movingActive ? REPORT_INTERVAL_FAST_S : REPORT_INTERVAL_POWERED_S;
  bool reportDue = millis() - lastReportMs >= poweredInterval * 1000UL;
  if (!doorChanged && !reportDue) return;

  awakeStart = millis();                                // re-arm bounded-op budget
  Sample s;
  readSensors(s);
  evaluateAlarm(s);
  lastSample = s; lastSampleValid = true;               // /status reads this copy
  gpsFreshThisWake = false;                             // powered loop never reboots — freshness is per report cycle
  // Powered-regime parity (2.90): sync the clock BEFORE the hunt so a fix persisted by
  // maybeGps carries this cycle's epoch (rtcEpoch is never aged in loop(), so it was
  // the last sync time — a stale timestamp on any backfill). The fix then rides this
  // cycle's frame; the broker echo below proves delivery and clears the NVS copy.
  { uint32_t e = networkEpoch(); if (e) rtcEpoch = e; }
  mqttEchoSeen = false; pendingFixSent = false;
  maybeGps(GPS_FIX_TIMEOUT_S);
  if (moveAlertPending && mqtt.connected()) {           // movement detected by that fix
    publishAlert(s, rtcEpoch, "moving");
    moveAlertPending = 0;
    saveMonState();
  }
  // GNSS-wedge recovery (powered regime only — battery self-heals via the
  // deep-sleep rail drop). A continuously-powered modem never loses the rail,
  // so when the watchdog reports the engine stuck, force a full power cycle
  // (the one thing that clears it) and re-establish the session.
  if (gnssZeroSatStreak >= GNSS_STUCK_CYCLES) {
    logLine("[gps] engine wedged %u cycles - full modem power cycle", gnssZeroSatStreak);
    gnssZeroSatStreak = 0;
    if (mqtt.connected()) mqtt.disconnect();
    modemPowerOn();                                     // true supply cut clears GNSS
    if (!modemConnect()) { goToSleep(REPORT_INTERVAL_FAST_S); }  // couldn't recover -> battery-style cycle
    char clientId[48];
    snprintf(clientId, sizeof(clientId), "freezermon-%s", deviceId);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
      char cmdTopic[64];
      snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/cmd", deviceId);
      mqtt.subscribe(cmdTopic);
      snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/telemetry", deviceId);
      mqtt.subscribe(cmdTopic);                         // echo subscription (delivery proof)
    } else {
      goToSleep(REPORT_INTERVAL_FAST_S);
    }
    lastReportMs = millis();                            // skip this cycle's report; resume next loop
    return;
  }
  uint32_t now = networkEpoch();
  if (now) rtcEpoch = now; else now = rtcEpoch;
  int csq = modem.getSignalQuality();
  int16_t rssiDbm = (csq >= 0 && csq < 99) ? (int16_t)(-113 + 2 * csq) : -999;

  bool ok = mqtt.connected() &&
            publishSample(s, now, false, doorChanged ? "door" : "powered", rssiDbm);
  if (ok) {                                             // delivery proof: our retained frame comes back on the echo subscription
    uint32_t tEcho = millis();
    while (millis() - tEcho < 3000UL && !mqttEchoSeen) { mqtt.loop(); delay(20); }
    if (mqttEchoSeen) clearPendingFix();                // the fix in that frame is in the broker — nothing to backfill
    else logLine("[mqtt] no echo of the powered frame - fix kept for backfill");
  }
  if (doorChanged && s.doorOpen) publishAlert(s, now, "door_open");
  if (tempAlertPending) { publishAlert(s, now, "temp_breach"); tempAlertPending = 0; saveMonState(); }
  if (coldChargeCheck(s))        publishAlert(s, now, "cold_charge");
  prevAlarm = s.alarm;
  lastDoor = door;
  lastReportMs = millis();

  if (!ok) {                                            // link lost -> recover via sleep cycle
    bufferSample(s);
    goToSleep(REPORT_INTERVAL_FAST_S);
  }
  if (!s.extPower) {                                    // power pulled -> battery regime
    goToSleep(s.alarm || s.doorOpen || movingActive ? REPORT_INTERVAL_FAST_S : REPORT_INTERVAL_S);
  }
}
