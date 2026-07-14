# freezerMon

Cellular monitoring for a mobile cooling unit. A LilyGO **T-A7608E-H** (ESP32 + SIMCom A7608E-H LTE Cat-4) reads temperature, door, battery/power-source and GPS, and publishes JSON over **LTE → MQTT**. A self-hosted stack (**Mosquitto → Telegraf → InfluxDB → Grafana**) stores it, draws dashboards, and fires alerts.

```
[DS18B20 x2]──┐
[reed switch]─┤  T-A7608E-H  ──LTE──►  Mosquitto ──► Telegraf ──► InfluxDB ──► Grafana
[battery/VIN]─┘  (deep sleep,                        (MQTT)        (ingest)     (store)     (dash + alerts)
                  offline buffer, GPS)
```

## What the device reports

Two operating regimes: on **battery** the device deep-sleeps between reports (default 5 min; 1 min during alarm/door-open) and wakes instantly on door-open via interrupt. On **external power** it never sleeps — the LTE session stays up, the door is polled every 250 ms (instant alerts), and it reports every 2 min. Payload, retained on `freezer/<id>/telemetry`:

| field | meaning |
|---|---|
| `ts` | unix epoch (from cellular network clock) |
| `t_cab`, `t_amb` | DS18B20 cabinet / ambient °C (second probe optional) |
| `door` | 1 = open (also triggers an instant wake + publish via interrupt) |
| `ext_power` | 1 = running on external power, 0 = on battery |
| `vbat_mv`, `vsolar_mv` | battery / VIN-solar millivolts |
| `rssi_dbm` | LTE signal |
| `lat`, `lon` | GPS fix (refreshed every Nth report to save power) |
| `alarm` | 1 = temp breach active (N consecutive over-threshold samples) |
| `buffered` | 1 = backfilled sample from an offline gap |

Alerts (`freezer/<id>/alert`): `temp_breach`, `door_open`, `batt_low`.
No-coverage readings are buffered in RTC memory (24 samples) and backfilled with corrected timestamps on reconnect.

## Hardware & wiring

| Part | Connection |
|---|---|
| DS18B20 probe(s) | data → **GPIO21**, 4.7 kΩ pull-up to 3V3; VCC 3V3; GND |
| Reed switch (door) | between **GPIO32** and **GND** (closed door = closed switch) |
| Battery | 18650 in on-board holder |
| External power | 5 V to the board's VIN/solar input (also powers `ext_power` detection via GPIO34) |
| SIM | nano-SIM, data plan, PIN disabled (or set `SIM_PIN`) |

> Pin map is for the standard T-A7608E-H. Board revisions exist — if the modem won't answer, verify pins against the [LilyGO-T-A76XX](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX) `utilities.h` for your exact board.

## Firmware (PlatformIO)

```bash
# ONE-TIME: generate YOUR OTA image-signing keypair (run from the repo root).
# Writes firmware/include/ota_pubkey.h (public, committed) + infra/keys/ (private,
# gitignored). The repo ships a placeholder key you MUST replace — otherwise you
# can't sign updates and your fleet would trust someone else's key.
bun infra/scripts/GenSigningKey.ts

cd firmware
cp include/config.example.h include/config.h   # then edit (see below)
pio run -e t-a7608-tls -t upload               # build + flash over USB (TLS build)
pio device monitor                             # watch the first cycle
```

**`config.h` is gitignored** (it holds credentials); the committed **`config.example.h`** is the template — copy it and replace every placeholder value:

| Setting | What to put |
|---|---|
| `DEVICE_ID` | unique id per unit (used in MQTT topics) |
| `APN` (+ `GPRS_USER`/`GPRS_PASS`, `SIM_PIN`) | from your SIM carrier — MVNOs often need user/pass |
| `MQTT_HOST` / `MQTT_HOST_IP` / `MQTT_USER` / `MQTT_PASS` | your broker + the credentials you created in `mosquitto_passwd` |
| `OTA_MANIFEST_URL` | your fw host + the `OTA_PATH_TOKEN` from the server `.env` |
| `DEBUG_AP_PASSWORD` | WPA2 password for the on-device console AP — **unique + high-entropy per unit** |

Notes:
- Uses the **lewisxhe TinyGSM fork** (declared in `platformio.ini`) — stock TinyGSM has no A76XX support.
- APN: ask your carrier; MVNOs often need `GPRS_USER`/`GPRS_PASS`.
- Sleep is guaranteed: a 3-minute awake guard forces deep sleep even if LTE hangs.

## Platform (any Docker host / VPS)

```bash
cd infra
cp .env.example .env        # fill every value (openssl rand -hex 24 for tokens)

# create MQTT credentials (matches MQTT_USER/MQTT_PASSWORD in .env):
docker run --rm -v ./mosquitto/config:/cfg eclipse-mosquitto:2.0 \
  mosquitto_passwd -c -b /cfg/passwd freezer '<your MQTT_PASSWORD>'

# the file is created root-owned; the broker drops privileges and must be able to read it:
docker run --rm -v ./mosquitto/config:/cfg eclipse-mosquitto:2.0 \
  sh -c 'chown mosquitto:mosquitto /cfg/passwd && chmod 640 /cfg/passwd'

docker compose up -d
```

Grafana: `http://<host>:3000` → **FreezerMon** folder → dashboard shows temperature, power source, door, battery, signal, and a location map. Firewall: expose only **1883** (devices) and **3000** (you) — InfluxDB stays internal.

### Logins

Two accounts, created like this:

| login | role | purpose |
|---|---|---|
| `admin` | Admin | you — full control (password: `GRAFANA_ADMIN_PASSWORD`) |
| `operator` | Viewer | read-only dashboards for whoever runs the unit (password: `GRAFANA_OPERATOR_PASSWORD`) |

```bash
cd infra && set -a && source .env && set +a && bun scripts/CreateOperator.ts
```

## Alerting

Three rules are provisioned: cabinet temp above −12 °C for 10 min, no data for 15 min (offline/out of coverage), battery under 3.4 V.
Point them at your phone: Grafana → Alerting → Contact points → add **Telegram** (bot token + chat id) or email, then set it as the default notification policy. If your Grafana version rejects the provisioned rule schema, recreate the three rules in the UI — the Flux queries in `infra/grafana/provisioning/alerting/alerts.yml` copy-paste directly.

## Field debugging (WiFi console)

The device broadcasts its own WiFi access point — `freezermon-<DEVICE_ID>`, password `DEBUG_AP_PASSWORD` from `config.h` (change the default!) — for on-site debugging with no cellular dependency:

- **After a cold boot** (power-on/reset): console stays up for `DEBUG_AP_WINDOW_S` (default 10 min) before the first deep sleep — connect while installing.
- **On external power**: console is always available.
- **On battery timer/door wakes**: WiFi stays off — zero power cost in normal operation.

Connect to the AP, then: `http://192.168.4.1/status` (live readings + state, JSON), `http://192.168.4.1/log` (recent event log: boot reason, LTE attach attempts, MQTT results, GPS, sleep decisions), and `http://192.168.4.1/sms` (SMS inbox — read SIM activation/confirmation texts without a phone). WiFi is never used for telemetry — LTE remains the only transport.

### OTA updates

**Over LTE (primary)** — no site visit at all:

```bash
# 1. bump FW_VERSION in firmware/src/main.cpp, build:
pio run -e t-a7608-tls
# 2. publish (uploads image to the server, sets the retained command):
bun infra/scripts/PublishFirmware.ts 1.4 cooler-01
```

The device checks `freezer/<id>/cmd` (retained) after every report; when `ota_ver` differs from its own version it streams the image from `https://freezer.shitshow.it/fw/<token>/…` over TLS into the inactive OTA slot and reboots — the next telemetry's `fw` field confirms the new version. Guard-rails: on battery it only updates above `OTA_MIN_VBAT_MV` (default 3.7 V; deferred otherwise until charged or powered), the watchdog stays fed during the ~2–4 min 2G download, and an interrupted transfer leaves the running firmware untouched. Data cost: ~0.9 MB per update.

**Over the debug WiFi (fallback)** — `http://192.168.4.1/update`: upload `firmware.bin` from a browser while connected to the debug AP (cold-boot window or external power). Same dual-slot safety.

## Data usage (fits easily in a 5 GB plan)

| Regime | Per report | Monthly |
|---|---|---|
| Battery, 5-min cadence, plain MQTT | ~2–3 KB (fresh TCP+MQTT session each wake) | ~25–30 MB |
| Battery, 5-min cadence, TLS | ~8 KB (handshake each wake) | ~70–90 MB |
| External power, 2-min cadence | ~0.5 KB (persistent session) | ~15 MB |

GPS is received, not transmitted — it costs no data. Worst case (TLS on battery) uses **under 2 %** of a 5 GB monthly allowance; a 5 GB plan gives ~50× headroom.

## Security

- Broker requires authentication; the stack refuses to start with unset secrets (`${VAR:?}`).
- **TLS upgrade path**: put certs (e.g. Let's Encrypt) in `infra/mosquitto/config/certs/`, uncomment the 8883 listener in `mosquitto.conf`, publish port 8883, then flash the device with `pio run -e t-a7608-tls -t upload` and set `MQTT_PORT 8883` in `config.h`. Note: the fork's secure client encrypts but does **not** validate the server certificate by default — for full MITM protection load a CA cert via the modem's certificate store (see the fork's SSL examples).
- `config.h` and `.env` are gitignored; only `.example` templates are committed.

## Power expectations

Deep sleep with the modem fully off dominates the budget. At a 5-min cadence expect roughly 2–4 weeks on a single 18650 (LTE attach ≈ 10–30 s at ~100+ mA per cycle is the main cost; GPS every 6th cycle). On external power the unit simply reports faster.

## Production deployment (Traefik)

Deployed at `root@192.168.201.50:/opt/freezermon` (host "FyretApp") using `infra/docker-compose.traefik.yml` — Grafana published as `https://freezer.shitshow.it`, MQTT as TLS on `mqtt.shitshow.it:8883` (Traefik `mqtts` entrypoint terminates TLS; certs via Let's Encrypt once DNS exists). Secrets live in `/opt/freezermon/.env` on the server. Device config for production: `MQTT_HOST "mqtt.shitshow.it"`, `MQTT_PORT 8883`, flash env `t-a7608-tls`.
