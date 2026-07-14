# Cellular OTA Investigation — LilyGO T-A7608E-H

**Status: ✅ WORKING (2026-07-14).** Cellular OTA self-update proven end-to-end —
the device pulled a full ~900 KB image over LTE, MD5-verified it, installed, and
rebooted into the new version (2.44 → 2.45, all 32 pieces, retries absorbed).

## The working recipe (TL;DR)

1. **Split the image into overlapping pieces** (`PIECE = 32768`, `OVERLAP = 4096`,
   `STEP = PIECE − OVERLAP = 28672`). Piece *i* = `image[i*STEP : i*STEP + PIECE]`;
   the short final piece is zero-padded by `OVERLAP`. Ship `ota_skip = OVERLAP`,
   `ota_size`, and `ota_md5` in the retained cmd.
2. **Device reads only the deliverable prefix** `min(plen − ota_skip, remaining)`
   of each piece — this dodges the modem's **3072-byte tail-drop** (see below), and
   the overlapping prefixes tile the image with no gap.
3. **Stage each piece fully in RAM, always reading from offset 0**, and commit to
   flash only after a clean read. On any read failure, **re-fetch and re-read the
   whole piece from 0** (up to `MAX_TRIES`=10). Never resume at a non-zero offset — the modem's
   offset-read after a re-fetch returns wrong bytes and corrupts the image.
4. **MD5-verify the whole image** (`Update.setMD5`) so a corrupt assembly is
   rejected, never installed.

Implementation: `firmware/src/main.cpp` → `performOta()`; `infra/scripts/PublishFirmware.ts`.

## ⚠️ Debugging gotcha that cost hours

The public proxy container is named **`traefik`** and the OTA backend is
**`freezermon-ota-1`** (nginx). Tailing a *non-existent* `freezermon-traefik-1`
returned empty and looked exactly like "the device isn't fetching" — when it was.
**Watch `docker logs freezermon-ota-1`** for real OTA fetch activity. Also: the
server's own shell resolves `freezer.shitshow.it` via a hairpin that 404s; test the
public URL from an external host (or the device) instead.

---

## Goal

Let `cooler-01` self-update its ~900 KB firmware image over the cellular link,
so we never have to physically UART-flash a deployed unit again. The device pulls
a new image when the retained MQTT command on `freezer/cooler-01/cmd`
(`{ota_url, ota_ver, ota_size}`) names a version different from its own
`FW_VERSION`, provided battery ≥ `OTA_MIN_VBAT_MV` (3700 mV).

Everything **except** the over-the-air image transfer works: telemetry, MQTT over
TLS, GNSS, the retained-command trigger, the split-image server pipeline, and
browser/WiFi flashing of the same image.

## The root constraint

The firmware is compiled with **`TINY_GSM_MODEM_A76XXSSL`** because MQTT-over-TLS
needs the modem's **CCH/SSL** application stack. That locks us out of the plain
**CIP TCP/IP** stack (`AT+NETOPEN`/`CIPOPEN`/`CIPRXGET`) that virtually every
working TinyGSM OTA example — including the reference sketch we were sent — relies
on. The entire investigation has been an effort to move ~900 KB through the modem
using only the services available alongside CCH: the modem's **HTTP** application
(`AT+HTTPINIT`/`HTTPACTION`/`HTTPREAD`) and the CCH channel itself.

## What the A7608 modem actually does (hard-won facts)

- **No `HTTPREADFILE` / `FSATTRI` / `CFTRANTX`.** Those file-system HTTP commands
  are A7670E-only; on the A7608 they return `ERROR`.
- **HTTP response cache ≈ 57 KB.** A single `HTTPACTION` GET larger than this
  can't be fully read back with `HTTPREAD`.
- **Won't cache `206 Partial Content`.** Ranged GETs (Range header via
  `HTTPPARA "USERDATA"`) return `206` with `content-length 0` to `HTTPREAD`.
- **`HTTPREAD` streams in ~4 KB chunks.** One `AT+HTTPREAD=<off>,<size>` call
  emits one *or more* `+HTTPREAD: <n>` data blocks (≈4 KB each) terminated by
  `+HTTPREAD: 0`. Non-zero offsets do work.
- **CCH plain-TCP reads stall ≈ 344 KB.** The CCH RX path wedges partway through a
  large streamed download; keep-alive second requests and CCHSTART-after-CCHSTOP
  reconnects fail; sharing the CCH channel with the live MQTT session corrupts reads.

## Approaches tried, in order

| # | Approach | Firmware | Result |
|---|----------|----------|--------|
| 1 | Stream image over the **CCH** TCP channel (mux 1) | early | Stalls ≈344 KB; no keep-alive; reconnect fails; MQTT/OTA channel-sharing corrupts reads. **Dead end.** |
| 2 | Pull to **modem filesystem** (`HTTPREADFILE`/`FSATTRI`) | 2.24–2.27 | A7608 lacks these commands (`rf:ERROR`, `fs:ERROR`). **Dead end.** |
| 3 | Single GET, read back from **HTTP cache** with `HTTPREAD` | 2.28–2.29 | Reads to ≈58 KB then `ERROR` — 57 KB cache ceiling. **Dead end for a full image.** |
| 4 | **Ranged GETs** (Range header) to page the image | 2.30–2.31 | `206`, `content-length 0` — modem won't cache partials. **Dead end.** |
| 5 | **Split image into 48 KB pieces** server-side; each piece a plain `200` GET that fits the cache; `HTTPREAD` each piece out and concatenate into the OTA slot | 2.32 → 2.38 | Piece *download* works reliably (Traefik confirms `200`/49152 B). The read-out is where the remaining bugs lived — see below. |

### The piece-split read-out saga (approach 5)

The server pipeline (`infra/scripts/PublishFirmware.ts`) splits `firmware.bin`
into `firmware.bin.000`, `.001`, … (48 KB each, under the 57 KB cache) and the
device fetches them sequentially, `HTTPREAD`-ing each into `Update.write()`.
Downloads always worked; every failure was in reading bytes out of the modem into
flash:

| Firmware | Failed at | Cause | Fix applied |
|----------|-----------|-------|-------------|
| 2.32 | byte **4096** | Read only the *first* `+HTTPREAD` chunk per call, then skipped the rest → stream desync, next read errored | Read **all** `+HTTPREAD:` chunks per call until `+HTTPREAD: 0` → 2.34 |
| 2.34 | byte **46080** | `RD=16384` makes the modem burst four 4 KB chunks back-to-back; while `Update.write()` stalled on a flash-sector erase, the burst overran the **256-byte default UART RX buffer**, dropping bytes | Enlarge `SerialAT` RX buffer to 32 KB → 2.36 |
| 2.36 | **byte 0** (no GET at all) | The 32 KB RX buffer starved the heap; `performOta`'s `malloc(16384)` failed under the live WiFi AP + MQTT TLS, so OTA aborted (`[ota] no buf`) **before its first request** — server saw zero fetches on every wake | `RD=4096` (modem sends one chunk then waits — no burst possible), 8 KB RX buffer, **static** OTA buffer (no `malloc` to fail); log free heap → 2.38 |
| 2.38 (48 KB pieces) | byte **46080** | Modem drops the tail of the cached response (see below) | Thought to be a ~46 KB cache wall → shrink pieces to 16 KB |
| 2.38 (16 KB pieces) | byte **13312** | **Same bug, smaller piece** — `16384 − 3072`, exactly as `49152 − 3072 = 46080` | Refined diagnosis: fixed **3072-byte tail-drop**; needs a firmware fix, not a piece-size change |

### The 3072-byte tail-drop (the actual root cause)

Two `/log` captures with different piece sizes were decisive:

```
# 48 KB pieces:              # 16 KB pieces:
[ota] 2.39 ... heap 219872   [ota] 2.39 ... heap 220564
[ota] read err p0 @46080     [ota] read err p0 @13312
[ota] failed at 46080/…      [ota] failed at 13312/…
```

Line them up: `49152 − 46080 = 3072` and `16384 − 13312 = 3072`. **The failure is a
fixed 3072-byte deficit from the end of the piece, independent of piece size.** The
modem reports the full `Content-Length` in `+HTTPACTION` but only ever caches
`length − 3072`, then `ERROR`s (it hands back a final partial ~1 KB chunk and quits).

This kills the "shrink the pieces" idea: **every piece loses its last 3072 bytes**,
so no split size helps — 16 KB failed *sooner* (13312), not later. It also corrects
the earlier "~46 KB wall" reading (46080 was just `48 KB − 3072`) and the "58368"
mark from 2.28.

Also ruled out along the way: `heap 219872/220564` → **220 KB free** (not heap);
identical deficit across two different images/code (not overflow or data-dependent);
`reset=poweron` and the unit on **USB + 18650** (not brownout — the "reboot loop" was
a misread of the telemetry `boot` field, which counts *packages since boot*, not
reboots; the device was stable throughout).

**Fix requires a firmware change** (no longer server-side-only):

- **Overlap + tail-skip.** Server overlaps pieces by ≥4 KB and pads the final piece;
  firmware reads only `plen − 4096` per piece (skipping the undeliverable tail) and
  writes exactly `ota_size` bytes total. Bounded change, directly defeats the drop.
- **CIP-stack rework.** Bypass the modem's HTTP app entirely (raw
  `NETOPEN`/`CIPOPEN`/`CIPRXGET`); avoids the tail-drop *and* the four earlier HTTP-app
  bugs. More work, most robust.

**Secondary open question:** the device attempts OTA reliably on a **power-on boot**
but was not observed attempting on **timer wakes** — no gate for that is visible in
the wake handler.

## Corroboration from the field

This is a well-known SIMCom limitation, not something peculiar to our setup — and
independent reporters landed on the same workaround we built.

- **SIMCom's own AT manual** documents the mechanism: *"The maximum size of protocol
  stack is 64K bytes for most modules (the CAT4 module is 10K bytes), and when the
  total size of the data from server is bigger than that and 'READMODE' is 0, you
  should read the data quickly or you will fail to read it,"* and *"the size of HTTP
  server response content should be shorter than 1M."* The A7608 is a Cat-4 module —
  the family called out as having the *smaller* buffer. The exact figure is
  firmware/SKU-dependent (nominal 64K, "Cat-4" 10K, ours ~46K). `READMODE`/"read
  quickly or lose it" also explains our 344 KB CCH streaming stall.
- **[LilyGO Modem-Series #352](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/352)**
  (SIM7670G OTA) reproduces all three of our symptoms: full-image GET fails with
  `+HTTPACTION: 0,706,0` (insufficient storage); *"after ~10 KB, attempting to set a
  new range always results in an error"* → *"the only way to continue is to restart
  the HTTP session"*; past ~100 KB, responses report the right length but return no
  payload — *"even with the official example code."*
- **[Arduino Forum — HTTPREAD buffer size](https://forum.arduino.cc/t/how-can-we-change-simcom-at-command-httpread-response-buffer-size/1122235)**
  confirms HTTPREAD fragments its output into fixed chunks (1024 B there, ~4 KB here)
  with repeated `+HTTPREAD:` headers and **no AT command to change it** — the exact
  behaviour that broke our fw 2.32.

The community consensus fix — split into sub-buffer chunks and **restart the HTTP
session per chunk** (`HTTPTERM`/`HTTPINIT`) — is precisely our `firmware.bin.NNN`
approach with a fresh session per piece. Range-header paging is widely reported as
unreliable (the `206` / "new range errors" problem — matches our `206, len 0`), so
separate small `200` GETs are the robust path. Sibling chips wall as low as 10 KB,
which is why we chose 16 KB pieces rather than staying near our measured ~46 KB.

## The flash-then-publish testing dance

Because the OTA read logic runs *on the device*, any fix to it must be UART-flashed
before it can be exercised — the running (buggy) firmware can't self-update using a
fix that only exists in the new image. So each round is: flash version *X* over
UART (which contains the fix), publish a cmd for *X* (so the freshly-flashed unit
doesn't see a stale older cmd and try to **downgrade**), then publish *X+1* to test
whether *X*'s OTA path pulls it. That's why the version numbers climb two at a time.

## Current device state

- Running **fw 2.38**, healthy and **stable**: MQTT/TLS up, telemetry flowing,
  `vbat ≈ 4290 mV` and rising (charging on USB + 18650), GNSS fix present. There
  was **no reboot loop** — that was a misread of the telemetry `boot` field (packages
  since boot, not reboots). It attempts OTA only on power-on boots, and each attempt
  fails cleanly at the tail-drop then continues; the unit is doing its job.
- Retained cmd **neutralised to 2.38** (matches current fw → no OTA attempts) since
  16 KB pieces don't fix the tail-drop. Re-arm 2.39 only alongside a real fix.
- All non-OTA functionality is production-ready.

## Options for when this is unparked

1. **Overlap + tail-skip firmware fix (one flash).** Server overlaps pieces by ≥4 KB
   and pads the last piece; firmware reads `plen − 4096` per piece and writes exactly
   `ota_size` bytes. Bounded change that directly defeats the 3072-byte tail-drop.
2. **CIP-stack OTA path (bigger flash).** Raw `AT+NETOPEN`/`CIPOPEN`/`CIPRXGET`,
   bypassing the modem's HTTP app (and all five of its bugs) for the download while
   MQTT keeps CCH. The stack the working reference sketch (`TINY_GSM_MODEM_A7608`)
   uses; most work, most robust.
3. **Accept UART/WiFi-only updates.** The unit is deployed and working; OTA is a
   convenience, not a functional requirement. Current resting state.

Secondary: **why timer wakes skip the OTA attempt** (only power-on boots attempt) —
no gate is visible in the wake handler; needs a fresh timer-wake `/log`.

## References

External (the tail-drop and its workaround):

- [SIMCom SIM7X00 HTTP AT Command Manual (PDF)](https://simcom.ee/documents/SIM7X00/SIM7500_SIM7600_SIM7800%20Series_HTTP_AT%20Command%20Manual_V1.00.pdf)
  and [A7600 Series AT Command Manual (PDF)](https://www.elecrow.com/wiki/image/d/dd/A7600_Series_AT_Command_Manual_V1.01.pdf)
  — the 64K/10K protocol-stack buffer + READMODE + 1M response-size spec.
- [LilyGO Modem-Series #352](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/352)
  — SIM7670G OTA: the 10 KB range-error / restart-session / >100 KB empty-read pattern.
- [LilyGO Modem-Series #443](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/443)
  — OTA over LTE (modem-FS and split approaches).
- [LilyGO Modem-Series #416](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series/issues/416)
  — GitHub-based OTA with SIM7670G.
- [Arduino Forum — changing SIMCom HTTPREAD buffer size](https://forum.arduino.cc/t/how-can-we-change-simcom-at-command-httpread-response-buffer-size/1122235)
  — HTTPREAD fixed-chunk fragmentation, not configurable.

Internal:

- Firmware OTA implementation: `firmware/src/main.cpp` → `performOta()`.
- Server publish pipeline: `infra/scripts/PublishFirmware.ts`.
- Plain-HTTP `/fw/` routing: `infra/docker-compose.traefik.yml`.
