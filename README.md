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

- **After a cold boot** (power-on/reset): console stays up for `DEBUG_AP_WINDOW_S` (default 2 min) before the first deep sleep — connect while installing.
- **On external power**: console is always available.
- **On battery timer/door wakes**: WiFi stays off — zero power cost in normal operation.

Connect to the AP, then: `http://192.168.4.1/status` (live readings + state, JSON), `http://192.168.4.1/log` (recent event log: boot reason, LTE attach attempts, MQTT results, GPS, sleep decisions), and `http://192.168.4.1/sms` (SMS inbox — read SIM activation/confirmation texts without a phone). WiFi is never used for telemetry — LTE remains the only transport.

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

## Production deployment (Traefik)

To run it behind Traefik on a public host, use `infra/docker-compose.traefik.yml` and set **your own** domains in the router rules (the file ships `example.com` placeholders): e.g. one host for Grafana + the OTA `/fw/` path, and one for MQTT-over-TLS on the `mqtts` entrypoint — Traefik terminates TLS, with certs via Let's Encrypt once DNS points at the host. Keep secrets in the server's `.env`. Device config for production: `MQTT_HOST` = your MQTT domain, `MQTT_PORT 8883`, flash env `t-a7608-tls`.

## License

[MIT](LICENSE) © 2026 Peter Hamborg Haugaard.

Third-party components keep their own licenses — see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Note the firmware links **TinyGSM (LGPL-3.0)** and the **Arduino-ESP32 core (LGPL-2.1)**: weak copyleft, satisfied here because the full source is public and rebuildable.
