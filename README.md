# freezerMon

Cellular monitoring for a mobile cooling unit. A LilyGO **[T-A7608E-H](https://lilygo.cc/products/t-a7608e-h)** (ESP32 + SIMCom A7608E-H LTE Cat-4) reads temperature, door, battery/power-source and GPS, and publishes JSON over **LTE → MQTT**. A self-hosted stack (**Mosquitto → Telegraf → InfluxDB → Grafana**) stores it, draws dashboards, and fires alerts.

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
| `moving` | 1 = unit displaced > `MOVE_ALARM_M` from its parked position (GPS-based) |
| `buffered` | 1 = backfilled sample from an offline gap |

Alerts (`freezer/<id>/alert`): `temp_breach`, `door_open`, `batt_low`, `moving`, `cold_charge`.

**Cold-charge protection (`cold_charge`)**: Li-ion cells must not be charged below ~0 °C (lithium plating). The board's CN3065 charger has a TEMP protection input, **but it ships wired to GND — disabled** — so the firmware detects the condition instead: if external power is present while the second DS18B20 (`t_amb`, strap it to the cell) reads below `COLD_CHARGE_C` (0 °C), a one-shot `cold_charge` alert fires (re-arms with +2 °C hysteresis) — unplug or warm the cell. This is detection, not enforcement; for autonomous cutoff see [the CN3065 TEMP hardware mod](#hardware-fix-temperature-protected-charging-cn3065-temp-mod). No second probe fitted → the check is inert.

**Movement detection** (GPS-based — the board has no accelerometer): the unit anchors its position while parked; any fix more than `MOVE_ALARM_M` (150 m, above GPS scatter) from the anchor raises a one-shot `moving` alert and switches to **fast reporting (60 s) with a GPS fix every wake**, so the map tracks the unit live. After `MOVE_STOP_CYCLES` consecutive near-still fixes it re-anchors at the new spot and drops back to the normal cadence. Detection latency while parked is bounded by the GPS cadence (a fix every `GPS_EVERY_N_REPORTS` wakes, ~30 min) — a battery-friendly trade-off; wire a vibration sensor to a spare interrupt pin if you need instant wake-on-motion.
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
| `DEVICE_ID` | **seed** default name only — the live name is stored in NVS and set per-unit at runtime via the `/setname` console (survives OTA). One firmware image serves a whole fleet; leave `""` to auto-derive a unique `cooler-<chipid>` on first boot. |
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

Three rules are provisioned: cabinet temp above 8 °C for 10 min (cooler food-safety ceiling — set it *above* your unit's normal band and keep it in sync with `TEMP_ALARM_C` in `config.h`, or the alarm never clears and the fast reporting cadence drains the battery ~5×; a freezer would use e.g. −12), no data for 15 min (offline/out of coverage), battery under 3.4 V.
Point them at your phone: Grafana → Alerting → Contact points → add **Telegram** (bot token + chat id) or email, then set it as the default notification policy. If your Grafana version rejects the provisioned rule schema, recreate the three rules in the UI — the Flux queries in `infra/grafana/provisioning/alerting/alerts.yml` copy-paste directly.

## Field debugging (WiFi console)

The device broadcasts its own WiFi access point — `freezermon-<DEVICE_ID>`, password `DEBUG_AP_PASSWORD` from `config.h` (change the default!) — for on-site debugging with no cellular dependency:

- **After a cold boot** (power-on/reset): console stays up for `DEBUG_AP_WINDOW_S` (default 2 min) before the first deep sleep — connect while installing.
- **On external power**: console is always available.
- **On battery timer/door wakes**: WiFi stays off — zero power cost in normal operation.

Connect to the AP, then: `http://192.168.4.1/status` (live readings + state, JSON), `http://192.168.4.1/log` (recent event log: boot reason, LTE attach attempts, MQTT results, GPS, sleep decisions), `http://192.168.4.1/sms` (SMS inbox — read SIM activation/confirmation texts without a phone), and `http://192.168.4.1/setname` (name this unit). WiFi is never used for telemetry — LTE remains the only transport.

**Naming a unit.** `/setname` writes the device name to NVS, which is what drives the MQTT topics (`freezer/<name>/…`) and the InfluxDB `device` tag Grafana filters on. Because NVS survives OTA (only the app partition is rewritten), the name sticks across updates — so the deployment workflow for a fleet is: flash the *same* image to every unit, then give each one a unique name here. Valid names are lowercase letters, digits, and hyphens (1–21 chars — bounded so the `freezermon-<name>` AP SSID stays within the 32-char WiFi limit); setting one reboots the unit so every topic re-derives cleanly. Leaving `DEVICE_ID` blank in `config.h` auto-derives a unique `cooler-<chipid>` per unit.

### OTA updates

Updates are **cryptographically signed**: the device installs an image only if its RSA-2048/SHA-256 signature — taken over the **version + image**, so downgrades and tampering both fail — verifies against the public key baked in at build time. This holds even though the command and download channels aren't themselves authenticated. Model + threat analysis: [docs/HARDENING.md](docs/HARDENING.md); the (long) road to a reliable pull on this modem: [docs/OTA-INVESTIGATION.md](docs/OTA-INVESTIGATION.md).

**Over LTE (primary)** — no site visit at all:

```bash
# 1. bump FW_VERSION in firmware/src/main.cpp, build:
pio run -e t-a7608-tls
# 2. sign + split + publish (also sets the retained MQTT command):
bun infra/scripts/PublishFirmware.ts 2.54 cooler-01
```

`PublishFirmware.ts` signs the image, splits it into overlapping ~32 KB pieces (the A7608's HTTP stack can't hand back a ~900 KB response in one go — see the investigation doc), and sets a retained command on `freezer/<id>/cmd`. After each report the device compares `ota_ver`; if it's **strictly newer** (anti-rollback) it pulls the pieces over plain HTTP, reassembles and hashes them, verifies the signature, installs into the inactive OTA slot, and reboots — the next telemetry's `fw` field confirms it. Guard-rails: on battery it updates only above `OTA_MIN_VBAT_MV` (default 3.7 V), the watchdog stays fed, per-piece retries ride out cellular blips, and a bad / interrupted / unsigned image leaves the running firmware untouched. Data cost: ~1 MB per update.

**Over the debug WiFi (fallback)** — `http://192.168.4.1/update`: upload `firmware.bin` **and** `firmware.bin.sig` plus the version; it installs only if the signature verifies. Same dual-slot safety. (This is the trusted channel to establish the *first* signed build on a fleet — see HARDENING.md.)

### OTA download internals (piece-split + verification)

Getting a ~900 KB image through the A7608 reliably was the hard part. The modem's HTTP stack can't hand back a large response in one transfer:

- its HTTP response cache is only ~57 KB, so a single GET larger than that can't be read back in full;
- it **drops the last ~3072 bytes of every cached response** — it reports the full `Content-Length` in `+HTTPACTION` but only caches `length − 3072`, then `ERROR`s;
- it won't cache `206 Partial Content`, so ranged GETs come back with a 0-byte body;
- and streaming over the CCH TCP channel wedges at ~344 KB.

**Solution — split the image into small, overlapping packages.** `PublishFirmware.ts` slices `firmware.bin` server-side:

```
PIECE   = 32768                 # bytes per piece file (comfortably under the cache)
OVERLAP = 4096                  # > the 3072-byte tail-drop
STEP    = PIECE - OVERLAP        # = 28672 real bytes each piece contributes
piece_i = image[i*STEP : i*STEP + PIECE]        # firmware.bin.000, .001, …
```

Because pieces advance by `STEP` but span `PIECE`, consecutive pieces **overlap** by `OVERLAP`; the short final piece is zero-padded by `OVERLAP`. The device fetches each piece as a plain `200` GET (fits the cache), then reads only the **deliverable prefix** `want = min(plen − ota_skip, remaining)`. Since `ota_skip = OVERLAP = 4096 > 3072`, that read never reaches the dropped tail, and the prefixes tile the image with **no gap and no overlap in the written bytes** — sum of `want` over all pieces = `ota_size` exactly. (`ota_skip` travels in the retained command, so the overlap is tunable without a reflash.)

Two more device-side rules make it robust on cellular:

- **Read each piece from offset 0 into a RAM buffer, commit only on a clean read.** The modem's offset-resume after a re-fetch returns *wrong bytes*, so a mid-piece resume corrupts the image (byte count still reaches 100 %); we never resume — a failed read re-fetches and re-reads the whole piece from 0, up to `MAX_TRIES = 10`, which absorbs transient blips.
- Only after a piece reads cleanly is it `Update.write()`-n to the inactive OTA slot.

**Verification — nothing installs unchecked.** While the assembled bytes are written, the device maintains two running digests:

- a streaming **MD5** (`Update.setMD5`) — a cheap corruption pre-check, and
- a streaming **SHA-256** over `version ‖ 0x00 ‖ image`.

When the image is complete it fetches `firmware.bin.sig` (a 256-byte **RSA-2048** signature, padded past the tail-drop and read back the same way) and verifies it against the public key compiled into firmware (`mbedtls_pk_verify`, PKCS#1 v1.5 / SHA-256). The install gate is a single short-circuit:

```c
if (ok && done >= total && otaVerifySig(url, digest) && Update.end(true)) { /* reboot */ }
else Update.abort();
```

`Update.end(true)` — which sets the boot partition — runs **only** after the signature verifies over the digest, and only when the offered version is strictly newer (`verNewer`, anti-rollback). A truncated, corrupt, unsigned, tampered, or downgraded image is aborted and the running firmware is left untouched. Full derivation of the 3072-byte tail-drop and the dead ends that preceded this: [docs/OTA-INVESTIGATION.md](docs/OTA-INVESTIGATION.md).

## Data usage (fits easily in a 5 GB plan)

| Regime | Per report | Monthly |
|---|---|---|
| Battery, 5-min cadence, plain MQTT | ~2–3 KB (fresh TCP+MQTT session each wake) | ~25–30 MB |
| Battery, 5-min cadence, TLS | ~8 KB (handshake each wake) | ~70–90 MB |
| External power, 2-min cadence | ~0.5 KB (persistent session) | ~15 MB |

GPS itself is received, not transmitted. **Assisted GNSS** (`AT+CAGPS`) downloads a few KB of ephemeris at most every ~2 h (`AGPS_REFRESH_S`) so cold fixes land in seconds instead of timing out — a small, bounded cost. Worst case (TLS on battery) still uses **under 2 %** of a 5 GB monthly allowance; a 5 GB plan gives ~50× headroom.

## Security

Full model and the independent-review findings are in [docs/HARDENING.md](docs/HARDENING.md). In brief:

- **Signed OTA** is the primary control: every image is RSA-2048/SHA-256 signed and verified on-device (with the version bound in, for anti-rollback) before install — over both LTE *and* the WiFi `/update` form. So even though the MQTT command channel isn't broker-authenticated (the modem's TLS runs `authmode=0`), a MITM can't push forged, tampered, or downgraded firmware. **Generate your own signing key first:** `bun infra/scripts/GenSigningKey.ts` (the committed `ota_pubkey.h` is a placeholder).
- Broker requires authentication; the stack refuses to start with unset secrets (`${VAR:?}`). MQTT runs over TLS (port 8883) — the fork's secure client encrypts but doesn't validate the broker cert by default, so signed-OTA is what guarantees firmware integrity regardless; for full MQTT MITM protection, load a CA cert via the modem's certificate store.
- `config.h`, `.env`, and `infra/keys/` (the OTA **private** signing key) are gitignored; only `.example` templates and the **public** key are committed.
- Known follow-ups (see HARDENING.md): per-device credentials, and a unique high-entropy `DEBUG_AP_PASSWORD` per unit.

## Power expectations

Deep sleep with the modem fully off dominates the budget. At a 5-min cadence expect roughly 2–4 weeks on a single 18650 (LTE attach ≈ 10–30 s at ~100+ mA per cycle is the main cost; GPS every 6th cycle). On external power the unit simply reports faster.

### The 18650: voltage ladder

What the firmware does as `vbat_mv` falls:

| vbat | Behaviour |
|---|---|
| ~4200–3700 mV | Normal operation |
| < 3700 mV (`OTA_MIN_VBAT_MV`) | OTA installs **deferred** (no flash writes on a sagging supply); telemetry unaffected |
| < 3400 mV (`BATT_LOW_MV`) | `batt_low` alert on every report + the Grafana battery rule fires — recharge now |
| ~3300–3400 mV *under load* | Modem TX bursts sag the rail → **brownout resets** (see below); reporting gets ragged and lossy |
| ~2500–3000 mV | Cell protection (or the ESP32's own brownout detector) cuts — unit is silent; the Grafana *no-data-15-min* rule is your notification |

### Brownouts on battery (cold or tired cells)

The A7608's LTE bursts peak around **2 A**. A cold 18650 (this is a *cooling* unit — a cell living in the cold zone at 2–5 °C has markedly higher internal resistance) or a worn one can sag below the ESP32's brownout threshold under those bursts **even when its resting voltage looks healthy (4+ V)**. Symptoms, from mildest to worst:

- `/log` shows `reset=BROWNOUT` on boot lines;
- telemetry frames arrive with `wake:"power_on"` and `boot:1` **every time** — RTC memory is wiped each reset, so nothing survives between cycles: the boot counter, the temp-alarm breach streak (the alarm can never trigger), the movement anchor, and the offline buffer are all lost each wake;
- cycles die before publishing at all (irregular gaps in the data).

Mitigations, in order of effectiveness:

1. **Bulk capacitance across the battery input** — solder a **1000–2200 µF low-ESR electrolytic** (≥6.3 V; a 105 °C-rated part behaves better in the cold) directly across the 18650 holder pads. This is the classic SIMCom-board fix: it carries the millisecond-scale TX/inrush spikes that trigger most brownouts. It can *not* carry the multi-second attach current — if the cell sags under sustained load, capacitance only delays the reset (that league needs a ~1 F low-ESR supercap/LIC).
2. **Keep the cell out of the cold zone.** The DS18B20 probe is on a wire — mount the board + battery outside the cooler and only the probe inside. Removes the root cause.
3. **External power** where the unit rides in a vehicle anyway (also gives instant door alerts and faster reporting).
4. A healthy, genuine cell: high-drain rated (≥5 A), not an aged or counterfeit one.

Related: the board browns out on a PC USB port at power-on regardless of battery temperature — it needs a **≥2 A** supply or the 18650 for the modem's startup surge.

### Hardware fix: temperature-protected charging (CN3065 TEMP mod)

Li-ion cells must never be **charged below ~0 °C** (lithium plating → permanent capacity loss, eventual safety risk). The board's charger — a **CN3065** ([datasheet](https://raw.githubusercontent.com/SeeedDocument/Lipo_Rider_Pro/master/res/DSE-CN3065.pdf)) — has exactly the protection input needed: the **TEMP pin suspends charging whenever V(TEMP) < 46 % of VIN** and resumes automatically above it. But on this board **TEMP (pin 1) is wired straight to GND, which disables the function** — verified in the [T-A7608X schematic](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/blob/main/schematic/esp32/T-A7608X-V1.0.pdf) (page 3, charger block `U7`; the datasheet's application circuit shows the intended NTC wiring).

The mod — re-enable it as a **cold cutoff**:

1. **Lift TEMP (pin 1) of the CN3065** off its GND pad (fine iron or hot air — this is the only delicate step), or cut its trace to GND.
2. Wire this divider, with the **NTC strapped to the 18650 cell** (not the air):

```
  CN3065 VIN (5 V / solar side)
        │
     [ NTC 10 kΩ B3950 ]   ← thermally bonded to the cell
        │
        ├──────── TEMP (pin 1, lifted from GND)
        │
     [ R1 22 kΩ ]
        │
       GND
```

3. How it behaves: cold cell → NTC resistance rises → V(TEMP) falls below 46 %·VIN → **charging suspends**; the cell warms → resumes on its own. With 10 k B3950 + 22 k, the cutoff lands at ≈ **+1 °C**. The divider is ratiometric to VIN, so it stays correct as a solar input sags and recovers. Tune R1 for a different cutoff: `R1 = R_NTC(at cutoff) × 0.46/0.54`.

Notes: in this orientation the single threshold guards the **cold** end only (the stock hot-side protection was disabled anyway); pair it with the firmware `cold_charge` alert for visibility. If SMD pin-lifting isn't your thing, the no-surgery alternative is an inline charger module with its own NTC input ahead of the board, or simply keeping the cell out of sub-zero placement.

## Production deployment (Traefik)

To run it behind Traefik on a public host, use `infra/docker-compose.traefik.yml` and set **your own** domains in the router rules (the file ships `example.com` placeholders): e.g. one host for Grafana + the OTA `/fw/` path, and one for MQTT-over-TLS on the `mqtts` entrypoint — Traefik terminates TLS, with certs via Let's Encrypt once DNS points at the host. Keep secrets in the server's `.env`. Device config for production: `MQTT_HOST` = your MQTT domain, `MQTT_PORT 8883`, flash env `t-a7608-tls`.

## License

[MIT](LICENSE) © 2026 Peter Hamborg Haugaard.

Third-party components keep their own licenses — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Note the firmware links **TinyGSM (LGPL-3.0)** and the **Arduino-ESP32 core (LGPL-2.1)**: weak copyleft, satisfied here because the full source is public and rebuildable.
