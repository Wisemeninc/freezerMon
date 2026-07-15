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
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>         // NVS-backed device name (survives OTA; set via /setname)
#include <Update.h>
#include "mbedtls/md.h"          // streaming SHA-256 (version-stable md_* API)
#include "mbedtls/pk.h"          // RSA-2048 signature verify
#include "ota_pubkey.h"          // OTA_PUBKEY_PEM — image-signing public key
#include "deviceid.h"            // deviceNameValid(), chipSeedName() — host-testable
#include "geo.h"                 // geoDistM() — movement detection, host-testable

#define FW_VERSION "2.66"

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
  return true;
}

static void mqttCallback(char *topic, byte *payload, unsigned int len) {
  (void)topic;
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  const char *u = doc["ota_url"], *v = doc["ota_ver"];
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
volatile bool modemBusy = false;   // main thread owns the modem UART while set

// Runtime device identity. Resolved once per boot from NVS (survives OTA, since
// OTA only rewrites the app partition) so ONE firmware image can serve a whole
// fleet — each unit is named independently via the /setname console endpoint.
// DEVICE_ID in config.h is only the seed default for an un-provisioned unit.
char deviceId[32] = "cooler-01";   // safe placeholder until resolveDeviceId() runs

#define LOG_RING 96      // holds a full multi-chunk OTA trace in /log
static String  logRing[LOG_RING];
static uint8_t logHead = 0, logCount = 0;

// log to serial AND the ring buffer served at /log (no trailing newline in fmt)
static void logLine(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  SerialMon.println(buf);
  logRing[logHead] = buf;
  logHead = (logHead + 1) % LOG_RING;
  if (logCount < LOG_RING) logCount++;
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

static void modemPowerOn() {
  pinMode(BOARD_POWERON_PIN, OUTPUT);
  // TRUE supply cut before powering up — not just a reset. The A76XX GNSS
  // engine can wedge (powered, CGNSSPWR READY, but tracking 0 satellites) and
  // only dropping the modem rail clears it; a reset/CPOF does not. Battery
  // wakes already lose this rail in deep sleep, but a continuously-powered
  // (external-supply) unit never does — so force it here on every power-on.
  digitalWrite(BOARD_POWERON_PIN, LOW);
  delay(1200);                                    // let the rail fully drain
  // Precharge double-tap: slamming the drained rail on in one step draws an
  // inrush surge that browns out a marginal cell/holder (seen live 2026-07-15:
  // every wake's FIRST power-on died, the reboot's second attempt survived on
  // the now-precharged caps). Emulate that deliberately: a short first tap
  // charges the modem's bulk capacitance, the brief drop bounds the surge, and
  // the final enable then sees a much smaller di/dt. Costs 250 ms.
  digitalWrite(BOARD_POWERON_PIN, HIGH);          // tap 1: precharge the bulk caps
  delay(150);
  digitalWrite(BOARD_POWERON_PIN, LOW);
  delay(100);
  digitalWrite(BOARD_POWERON_PIN, HIGH);          // rail for modem section (caps precharged)

  pinMode(MODEM_RESET_PIN, OUTPUT);               // reset pulse (LilyGO reference)
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL); delay(100);
  digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);  delay(2600);
  digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

  pinMode(BOARD_PWRKEY_PIN, OUTPUT);              // PWRKEY: 1 s active pulse (100 ms is flaky on A76xx)
  digitalWrite(BOARD_PWRKEY_PIN, LOW);  delay(100);
  digitalWrite(BOARD_PWRKEY_PIN, HIGH); delay(1000);
  digitalWrite(BOARD_PWRKEY_PIN, LOW);

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

static bool readGnssFix(float *outLat, float *outLon, int *satsUsed);  // defined near maybeGps

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
  static const char *setnameHdr[] = { "Origin" };
  debugServer.collectHeaders(setnameHdr, 1);           // /setname CSRF same-origin check

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
    Sample s;
    readSensors(s);
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
    for (uint8_t i = 0; i < logCount; i++) {
      out += logRing[(logHead + LOG_RING - logCount + i) % LOG_RING];
      out += '\n';
    }
    debugServer.send(200, "text/plain", out);
  });
  debugServer.on("/lte", []() {
    debugServer.sendHeader("Refresh", "15");            // browser auto-reload while monitoring
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    if (modemBusy) {
      // main thread owns the modem UART right now (attach/publish in progress)
      doc["busy"] = true;
      doc["note"] = "modem attach/publish in progress - refresh in ~30s";
    } else if (poweredSession) {
      // powered regime: main loop services MQTT on the UART continuously —
      // report link state without injecting AT commands into that stream
      doc["powered"]        = true;
      doc["mqtt_connected"] = mqtt.connected();
      doc["note"] = "live modem queries suspended while MQTT session is held";
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
      modemBusy = true;                                   // hold the UART for the query burst
      doc["apn_cfg"]     = APN;                           // compiled-in APN
      doc["pdp_addr"]    = atQuery("+CGPADDR");           // context IP address(es)
      doc["pdp_dhcp"]    = atQuery("+CGCONTRDP");         // APN, local IP + subnet mask, gateway, DNS1, DNS2
      doc["pdp_active"]  = atQuery("+CGACT?");            // context activation state
      doc["pdp_define"]  = atQuery("+CGDCONT?");          // configured contexts / APNs
      doc["dns_lookup"]  = atQuery("+CDNSGIP=\"" MQTT_HOST "\"", 5000);  // resolve via the network-provided DNS
      modemBusy = false;
    }
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
    if (modemBusy) {
      doc["busy"] = true;
      doc["note"] = "modem attach/publish in progress - refresh in ~30s";
    } else if (poweredSession) {
      doc["powered"] = true;
      doc["note"] = "GNSS debug unavailable while MQTT session is held (UART owned by main loop)";
      doc["last_lat"] = lastLat;
      doc["last_lon"] = lastLon;
    } else {
      if (!gnssDebugOn)                                   // stays on while console is up
        gnssDebugOn = modem.enableGPS(GPS_ANTENNA_POWER_PIN, GPS_ANTENNA_POWER_LEVEL);
      doc["gnss_powered"] = gnssDebugOn;
      float lat = 0, lon = 0;
      int sats = 0;
      if (readGnssFix(&lat, &lon, &sats)) {              // anchor-based parse — fork's getGPS misreads this modem
        doc["fix"] = true;
        doc["lat"] = lat;
        doc["lon"] = lon;
        doc["sats"] = sats;
        doc["raw"] = atQuery("+CGNSSINFO");
        lastLat = lat; lastLon = lon;                    // console-triggered fix feeds the next report
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
    }
    String out;
    serializeJsonPretty(doc, out);
    debugServer.send(200, "application/json", out);
  });

  debugServer.on("/sms", []() {
    // SIM activation flows deliver a text — surface the inbox here so no
    // phone is needed to complete registration in the field
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
    if (debugServer.hasHeader("Origin") &&
        debugServer.header("Origin") != "http://192.168.4.1") {
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
#ifdef OTA_MANIFEST_URL
    otaCheckRequested = true;
    debugServer.send(200, "text/plain",
      "queued - the device fetches the manifest at its next modem-free moment; watch /log\n");
#else
    debugServer.send(503, "text/plain", "OTA_MANIFEST_URL not set in config.h\n");
#endif
  });
  debugServer.on("/update", HTTP_POST, []() {
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
      logLine("[ota] receiving %s (%s)", up.filename.c_str(), up.name.c_str());
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
        logLine("[ota] /update ver=%s", v.c_str());
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
  modemBusy = true;
  bool up = otaConnect(host, port);
  if (!up) {
    logLine("[ota] manifest connect failed");
    modemBusy = false;
    return;
  }
  otaHttpClient.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n");
  otaHttpClient.setTimeout(15000);
  String status = otaHttpClient.readStringUntil('\n');
  if (status.indexOf("200") < 0) {
    logLine("[ota] manifest http %s", status.c_str());
    otaHttpClient.stop();
    modemBusy = false;
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
  modemBusy = false;

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

static bool performOta(const char *url, const char *ver) {
  // Atomically claim exclusive flash access vs. a local /update flash (dbgweb task).
  // A stale otaUploadActive (dropped upload) is ignored after 3 min.
  portENTER_CRITICAL(&flashMux);
  bool busy = otaUploadActive && (millis() - otaUploadMs < 180000UL);
  if (!busy) modemBusy = true;                 // claim it before releasing the lock
  portEXIT_CRITICAL(&flashMux);
  if (busy) { logLine("[ota] deferred - local /update flash in progress"); return false; }
  Stream &at = SerialAT;
  const long RD = 4096;                  // one HTTPREAD chunk: the modem sends 4 KB
                                         // then waits for the next command, so it
                                         // never bursts past what we can drain — no
                                         // UART overflow during Update.write() stalls.
  long total = otaSize;                  // total image size, from the retained cmd
  modemBusy = true;
  logLine("[ota] %s size %ld heap %u (pieces)", ver, total, ESP.getFreeHeap());
  if (total <= 0) { logLine("[ota] no size in cmd"); modemBusy = false; return false; }
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
  if (!Update.begin(total)) { logLine("[ota] begin fail heap %u", ESP.getFreeHeap()); modemBusy = false; return false; }
  if (otaMd5[0]) Update.setMD5(otaMd5);  // verify the whole image; never install a corrupt one

  // Stream SHA-256 over (ota_ver ‖ 0x00 ‖ image) — the version is part of the signed
  // payload, so verNewer() below plus this binding stop a MITM from relabelling an old
  // signed image with a higher version to force a downgrade.
  mbedtls_md_context_t md; mbedtls_md_init(&md);
  if (mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) != 0 ||
      mbedtls_md_starts(&md) != 0) {
    logLine("[ota] sha init fail"); mbedtls_md_free(&md); Update.abort(); modemBusy = false; return false;
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
  while (done < total && ok) {
    char purl[224];
    snprintf(purl, sizeof(purl), "%s.%03d", url, piece);
    long pieceStart = done;                      // flash position where this piece begins
    bool pieceOk = false;
    for (int tries = 1; tries <= MAX_TRIES && !pieceOk && ok; tries++) {
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
  logLine("[ota] failed at %ld/%ld ok=%d uerr=%d", done, total, ok, uerr);
  modemBusy = false;
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
static bool readGnssFix(float *outLat, float *outLon, int *satsUsed) {
  if (satsUsed) *satsUsed = 0;
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
  if (satsUsed) {
    int sum = 0;
    for (int i = 1; i <= 4 && i < n; i++) sum += tok[i].toInt();
    *satsUsed = sum;
  }
  int ns = -1;
  for (int i = 1; i < n - 2; i++) {
    if ((tok[i] == "N" || tok[i] == "S") && (tok[i + 2] == "E" || tok[i + 2] == "W")) { ns = i; break; }
  }
  if (ns < 1 || !tok[ns - 1].length() || !tok[ns + 1].length()) return false;  // no fix yet
  float lat = tok[ns - 1].toFloat();
  float lon = tok[ns + 1].toFloat();
  // some firmware sends decimal degrees, some ddmm.mmmmmm — out-of-range
  // values can only be the latter, convert those
  if (fabsf(lat) > 90.0f)  { float d = floorf(lat / 100.0f); lat = d + (lat - d * 100.0f) / 60.0f; }
  if (fabsf(lon) > 180.0f) { float d = floorf(lon / 100.0f); lon = d + (lon - d * 100.0f) / 60.0f; }
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

static bool maybeGps(uint32_t fixTimeoutS) {
  // while moving, track every wake so the map follows the unit live
  if (!movingActive && reportsSinceGps < GPS_EVERY_N_REPORTS) { reportsSinceGps++; return false; }
  // Retry cadence, not fix cadence: reset the counter for the ATTEMPT. A
  // no-fix cycle (unit indoors) must wait N reports again — resetting only on
  // success made GNSS hunt 90 s on EVERY wake and starve the MQTT session.
  reportsSinceGps = 0;
  if (!modem.enableGPS(GPS_ANTENNA_POWER_PIN, GPS_ANTENNA_POWER_LEVEL)) { logLine("[gps] enable failed"); return false; }
  // Assisted GNSS: pull current ephemeris from SIMCom's AGNSS server over the (already
  // attached) data bearer so a cold start fixes in seconds instead of timing out at 90 s.
  // AGPS data stays valid a few hours → refresh at most every ~2 h to spare the SIM.
  // Best-effort: on failure we fall back to an unassisted cold fix, so it can only help.
  bool agpsFresh = rtcEpoch && lastAgpsEpoch && (rtcEpoch - lastAgpsEpoch) < AGPS_REFRESH_S;
  if (!agpsFresh) {
    modem.sendAT("+CAGPS");
    if (modem.waitResponse(20000L) == 1) { lastAgpsEpoch = rtcEpoch; saveMonState(); logLine("[gps] AGPS ephemeris loaded"); }
    else                                   logLine("[gps] AGPS load failed - unassisted cold fix");
  }
  float lat, lon;
  int sats = 0, maxSats = 0;
  bool gotFix = false;
  float prevLat = lastLat, prevLon = lastLon;           // previous fix, for movement detection
  uint32_t start = millis();
  while (millis() - start < fixTimeoutS * 1000UL && awakeBudgetLeft()) {
    bool fix = readGnssFix(&lat, &lon, &sats);
    if (sats > maxSats) maxSats = sats;
    if (fix) {
      lastLat = lat; lastLon = lon;
      gotFix = true;
      break;
    }
    delay(2000);
  }
  if (gotFix) { gpsFreshThisWake = true; updateMovement(prevLat, prevLon); }
  // GNSS-wedge watchdog: an engine that is powered but tracks 0 satellites the
  // whole window is stuck (only a supply cut recovers it). Seeing any sat —
  // even without a full fix — means it's alive, just acquiring.
  if (maxSats > 0) gnssZeroSatStreak = 0;
  else if (gnssZeroSatStreak < 255) gnssZeroSatStreak++;
  if (!gotFix) logLine("[gps] no fix (maxSats=%d, wedge streak=%u)", maxSats, gnssZeroSatStreak);
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
  }
  doc["alarm"]     = s.alarm;
  doc["moving"]    = movingActive;
  doc["boot"]      = bootCount;
  doc["buffered"]  = buffered ? 1 : 0;
  doc["wake"]      = wakeReason;
  doc["rst"]       = g_resetStr;      // reset reason (diagnosing the no-sleep loop)
  doc["ph"]        = prevPhase;       // how far the PREVIOUS cycle got (NVS breadcrumb, 5=reached sleep entry)
  doc["fw"]        = FW_VERSION;      // fleet version tracking + OTA confirmation

  char topic[64], payload[384];
  snprintf(topic, sizeof(topic), "freezer/%s/telemetry", deviceId);
  size_t n = serializeJson(doc, payload, sizeof(payload));
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

  // age buffered samples and the epoch estimate by the coming sleep window
  for (uint8_t i = 0; i < rtcBufCount; i++) rtcBuf[i].ageS += seconds;
  if (rtcEpoch) rtcEpoch += seconds + (millis() - awakeStart) / 1000UL;

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
  SerialMon.begin(115200);
  resolveDeviceId();     // NVS -> deviceId, before the AP SSID / MQTT topics use it

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
  { Preferences p; p.begin("freezermon", true); prevPhase = p.getUChar("ph", 0); p.end(); }
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
  if (coldBoot && rr != ESP_RST_BROWNOUT) startDebugAp();
  else if (coldBoot) logLine("[boot] debug AP skipped - brownout recovery cycle");

  Sample s;
  readSensors(s);
  evaluateAlarm(s);

  bool published = false;
  if (awakeBudgetLeft()) {
    modemBusy = true;                                   // /lte reports busy during attach
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

      mqtt.setBufferSize(512);
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
        published = publishSample(s, now, false, wakeReason, rssiDbm);

        if (tempAlertPending) { publishAlert(s, now, "temp_breach"); tempAlertPending = 0; saveMonState(); }
        if (doorWake && s.doorOpen)     publishAlert(s, now, "door_open");
        if (s.vbatMv > 0 && s.vbatMv < BATT_LOW_MV) publishAlert(s, now, "batt_low");
        if (coldChargeCheck(s))         publishAlert(s, now, "cold_charge");

        // OTA check: the retained cmd (if any) arrives within a moment of subscribing
        char cmdTopic[64];
        snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/cmd", deviceId);
        mqtt.subscribe(cmdTopic);
        uint32_t tCmd = millis();
        while (millis() - tCmd < 3000UL && !otaPending) { mqtt.loop(); delay(20); }
        if (otaPending) {
          if (s.extPower || s.vbatMv >= OTA_MIN_VBAT_MV) {
            performOta(otaUrl, otaVer);       // reboots on success
          } else {
            logLine("[ota] deferred - battery %umV below %d", s.vbatMv, OTA_MIN_VBAT_MV);
          }
          otaPending = false;
        }
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
      // rather than waiting for the next wake to carry lastLat/lastLon. Same
      // ts, so InfluxDB upserts the coords onto this cycle's point. Periodic
      // timer wakes keep the full window and the after-publish-only behaviour.
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
        } else {
          gotFix = maybeGps(coldBoot ? GPS_FIRST_BOOT_TIMEOUT_S : GPS_FIX_TIMEOUT_S);
        }
        // `now` guard: if the clock never synced this cycle, `ts` is omitted from
        // both frames and the same-ts upsert can't happen -> skip to avoid a
        // duplicate point (the fix still ships next wake via lastLat/lastLon).
        if (coldBoot && gotFix && published && now && mqtt.connected())
          publishSample(s, now, false, wakeReason, rssiDbm);
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
  }
  markPhase(3);                                         // modem work done

  if (!published) bufferSample(s);                      // data outlives connectivity

  // Deep sleep is a battery measure. On external power there is nothing to
  // protect: stay awake with the session up, poll the door continuously
  // (instant alerts instead of wake latency) and report on the powered cadence.
  if (s.extPower && published && mqtt.connected()) {
    poweredSession = true;
    lastDoor = s.doorOpen;
    prevAlarm = s.alarm;
    lastReportMs = millis();
    if (!debugApActive) startDebugAp();                 // console always on while powered
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
void loop() {
  if (!poweredSession) goToSleep(REPORT_INTERVAL_S);    // safety net

  esp_task_wdt_reset();
  mqtt.loop();
  if (Update.isRunning()) { delay(5); return; }         // OTA in progress — don't report or sleep
#ifdef OTA_MANIFEST_URL
  if (otaCheckRequested) {
    checkOnlineUpdate();                                // drops the MQTT session (shared client)
    if (!otaPending) {                                  // no update -> restore it in place
      char clientId[48];
      snprintf(clientId, sizeof(clientId), "freezermon-%s", deviceId);
      if (mqtt.connect(clientId, MQTT_USER, MQTT_PASS)) {
        char cmdTopic[64];
        snprintf(cmdTopic, sizeof(cmdTopic), "freezer/%s/cmd", deviceId);
        mqtt.subscribe(cmdTopic);
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
  gpsFreshThisWake = false;                             // powered loop never reboots — freshness is per report cycle
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
