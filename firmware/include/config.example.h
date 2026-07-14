// freezerMon device configuration
// Copy to config.h and fill in — config.h is gitignored, never commit credentials.
#pragma once

// ---- Identity ----
#define DEVICE_ID            "cooler-01"        // SEED default only — the live name lives in NVS and is
                                                //   set per-unit at runtime via the /setname console (survives OTA).
                                                //   Leave "" to auto-derive a unique cooler-<chipid> on first boot.

// ---- Cellular ----
#define APN                  "internet"          // Lebara DK: "internet" (verified live)
// Pin the network operator. Consumer MVNO SIMs (e.g. Lebara) may NEVER
// auto-select their home carrier — seen live: modem hunted foreign LTE and got
// denied while home coverage was available. 23866 = Telia-Telenor DK shared net.
// Comment out for automatic selection (IoT/roaming SIMs).
#define FORCE_PLMN           "23866"
#define GPRS_USER            ""
#define GPRS_PASS            ""
#define SIM_PIN              ""                  // leave empty if PIN lock disabled

// ---- MQTT ----
#define MQTT_HOST            "vps.example.com"
// Optional: broker IP, dialed first to bypass flaky carrier DNS on the modem.
// Requires a TLS front (e.g. Traefik HostSNI(`*`)) that routes SNI-less
// connections to the broker, and modem authmode=0 (no cert-CN check).
// Comment out to always use the hostname.
#define MQTT_HOST_IP         "203.0.113.10"
#define MQTT_PORT            1883                // set 8883 when flashing the TLS env
#define MQTT_USER            "freezer"
#define MQTT_PASS            "change-me"
// TLS: flash with `pio run -e t-a7608-tls -t upload` (defines USE_TLS) and set MQTT_PORT 8883

// ---- Reporting cadence ----
#define REPORT_INTERVAL_S       300              // normal (on battery): every 5 min
#define REPORT_INTERVAL_POWERED_S 120            // on external power: device stays AWAKE (no deep sleep) and reports at this cadence
#define REPORT_INTERVAL_FAST_S  60               // while alarm active or door open
#define GPS_EVERY_N_REPORTS     6                // GPS fix every Nth wake (~30 min)
#define GPS_FIX_TIMEOUT_S       90               // give up on fix after this
#define AGPS_REFRESH_S          7200             // re-download AGPS ephemeris at most this often (~2 h) for fast cold fixes
// GNSS active antenna power. T-A7608/T-A7670: 127 = modem VDD_AUX rail
// (AT+CVAUXS=1). Without this the antenna is dead — zero satellites forever.
#define GPS_ANTENNA_POWER_PIN   127
#define GPS_ANTENNA_POWER_LEVEL 1
// GNSS-wedge watchdog: consecutive 0-satellite GPS attempts before forcing a
// full modem power cycle (the A76XX GNSS engine can hang until a supply cut).
#define GNSS_STUCK_CYCLES       3

// ---- Alarm thresholds ----
#define TEMP_ALARM_C            -12.0f           // cabinet warmer than this = breach
#define ALARM_CONSECUTIVE       3                // consecutive breaches before alert
#define BATT_LOW_MV             3400
#define EXT_POWER_MIN_MV        4400             // VIN/solar ADC above this = external power present

// ---- OTA over LTE ----
#define OTA_MIN_VBAT_MV      3700               // on battery, only OTA above this charge
// Target of the /update "Check online for update" button. PublishFirmware.ts
// writes this manifest next to firmware.bin. Comment out to hide the button.
// Plain HTTP recommended: cellular modem TLS on a second session is often
// flaky; the unguessable token path is the access control, and Update.begin/end
// validate the image so a corrupt download fails safely.
#define OTA_MANIFEST_URL     "http://vps.example.com/fw/<token>/manifest.json"

// ---- Field debug console (WiFi AP) ----
#define DEBUG_AP_PASSWORD    "freezer-debug"    // CHANGE THIS — WPA2, min 8 chars
#define DEBUG_AP_WINDOW_S    120                // console window after cold boot on battery (s)

// ---- Timeouts / bounds ----
#define MAX_AWAKE_MS            180000UL         // hard guard: always sleep after 3 min
#define NET_ATTACH_TIMEOUT_MS   90000UL
#define CONNECT_ATTEMPTS        3

// ---- Pins (LilyGO T-A7608E-H — verify against your board revision) ----
#define MODEM_TX_PIN         26                  // ESP32 TX -> modem RX
#define MODEM_RX_PIN         27                  // ESP32 RX <- modem TX
#define BOARD_PWRKEY_PIN     4
#define MODEM_RESET_PIN      5                   // strapping pin — driven only after boot
#define MODEM_RESET_LEVEL    HIGH                // T-A7608: reset net is transistor-inverted
#define BOARD_POWERON_PIN    12                  // strapping pin — must be low at boot (it is: low in sleep)
#define MODEM_DTR_PIN        25
#define BOARD_BAT_ADC_PIN    35                  // behind 1:2 divider
#define BOARD_SOLAR_ADC_PIN  34                  // behind 1:2 divider
#define ONE_WIRE_PIN         21                  // DS18B20 data (4.7k pullup to 3V3)
#define DOOR_PIN             GPIO_NUM_32         // reed switch to GND, RTC-capable
