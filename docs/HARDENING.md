# freezerMon — Security Hardening

Started 2026-07-14, after cellular OTA was proven working. Threat model and
prioritized plan. The OTA path is the highest-value surface: a compromise there
means arbitrary code execution on the device.

## Threat model — findings, prioritized

| P | Finding | Status |
|---|---------|--------|
| **P0** | **OTA command channel is unauthenticated** (`authmode=0`). A MITM can push a malicious OTA cmd; MD5 gives no protection. | **✅ DONE — signed OTA (2.49)** |
| **P1** | **`/update` local flash accepts any image** (behind the shared WPA2 AP password). | **✅ DONE — signed `/update` (2.49)** |
| **P2** | **Downgrade allowed** (`ota_ver != FW_VERSION`) → old signed-but-vulnerable version can be pushed. | **✅ DONE — `verNewer()` + version bound into the signature (2.50)** |
| **P3** | **Secrets compiled into the binary** (MQTT + AP passwords). **Re-rated 2026-08-26: remotely extractable, not just via flash dump** — the OTA pieces are served over plain HTTP behind `OTA_PATH_TOKEN`, which travels in the clear on every fetch and inside the unauthenticated MQTT session. Note: the committed *example* `DEBUG_AP_PASSWORD` is weak — must be unique + high-entropy per unit. | **Open — per-device NVS-provisioned creds is the real fix.** Mitigated: unsplit `firmware.bin` no longer published; broker ACLs (P6) confine a leaked credential to telemetry/alert of the shared identity. |
| P4 | Info disclosure on the debug console (`/lte`, `/gps`, `/sms`, `/log`) — behind WPA2. **Re-rated 2026-08-26:** on externally-powered units the AP and every endpoint are up 24/7 (`main.cpp` "console always on while powered"), not for a 120 s window, behind one fleet-wide PSK. | Medium on powered units — on-demand AP + per-unit PSK pending |
| **P5** | **Availability: OTA pull was unbounded.** `performOta` fed the watchdog on every retry and never checked the awake budget; the retained `cmd` re-arms on every subscribe. A `cmd` pointing at a stalling server = hours awake per wake, forever, remotely (anyone who can write `cmd`, or the P0 MITM). | **✅ DONE (2.69)** — `MAX_OTA_MS` cap + NVS failure memory (`OTA_MAX_FAILS`) |
| **P6** | **No broker authorization.** One shared credential (devices + Telegraf) with no `acl_file` could write any `freezer/*/cmd` (fleet-wide P5) and forge any unit's telemetry (the Grafana breach rule reduces `last()`, so a forged low reading clears a real alert). | **✅ DONE for `cmd`** — `mosquitto/config/acl`: per-device `pattern %u` rules, shared device user cannot write `cmd`, read-only `telegraf`, `cmd`-only `publisher`. **Residual until P3:** the shared `freezer` credential can still publish to any `freezer/+/telemetry` / `/alert`, so a holder can forge another unit's readings (and clear its breach alert). Remove the `user freezer` block once every unit has its own credential. |
| **P7** | **Debug-console concurrency.** dbgweb task and main thread shared the modem UART, the flash writer and the log ring behind an advisory `modemBusy` bool that `/lte` cleared unconditionally and `/sms` ignored; `String logRing[]` was a use-after-free between `logLine` and `/log`. Reachable by anyone with the AP PSK. | **✅ DONE (2.69)** — recursive FreeRTOS mutex owns the UART (console try-takes, answers busy; `/sms` guarded), fixed `char` ring under a spinlock, `modemBusy` written by the main thread only and restored (not cleared) by nested OTA paths |
| P8 | CSRF on the console: `/setname` Origin check failed open on a missing header; `/update` and `/update/check` had no check — a drive-by page on the AP could erase the inactive OTA slot and block cellular OTA for 3 min. | **✅ DONE (2.69)** — `sameOriginRequest()` fail-closed on all three, enforced in the upload callback before `Update.begin` |
| P9 | `PublishFirmware.ts` built a remote root shell command by string concatenation from unvalidated argv (`JSON.stringify` is not shell quoting). | **✅ DONE** — argv validated against the firmware's own contracts, payload on stdin, `publisher` identity |

## Round 4 — gate on the 2.69→2.86 train (2026-08-27, opencode GPT-5.5 + Kimi K3, plus a Claude-family auditor)

Three reviewers that did not author the train (`cdb379f..14e528a`) confirmed all six round-3 HIGH fixes as real
(mutex-owned UART, char log ring, fail-closed CSRF hoisted before `Update.begin`, OTA cap + failure memory, ACL
`cmd`-write ban, hardened publish script). They found the train's own new surface wanting — all fixed in 2.87:
the pending-fix clear depended on an optional cmd downlink (a fleet with no retained OTA republished a fix forever)
→ now verified by the broker echoing the device's own retained frame; per-version OTA failure memory was evadable by
cycling `ota_ver` → global consecutive counter, reset only by a real install; `crash_log` shipped raw device/cmd
strings and could push a frame over the buffer and suppress a whole boot's telemetry → sanitized, budgeted, capped
at the log sites; `${device}` was raw-interpolated into 19 Flux queries (a Viewer could inject Flux via the URL) →
`${device:regex}`; `PublishFirmware.ts` sourced the server `.env` as root shell → read with `sed`; scoped InfluxDB
tokens replaced the shared admin token. **Recorded residuals:** the shared `freezer` credential can still forge any
unit's telemetry/alert until per-device credentials (P3); Mosquitto `pattern` rules also grant service identities
write on `freezer/<their-username>/*`; `/gps` and `/lte` are GET endpoints with modem side effects behind the AP PSK.

## Round 3 — full-codebase `/independent-review` gate (2026-08-26)

Two reviewers that did not author the code (`Cato` correctness/design, `Silas` security) audited the
entire tree at `cdb379f`. Both independently confirmed the signed-OTA core: exactly two `Update.end(true)`
sites, both behind `otaCheckSig` over `version‖0x00‖image`; anti-rollback correct; piece reassembly bounded;
`deviceNameValid` blocks topic/SSID injection; no secrets in tracked files. Verdict was **CHANGES REQUESTED**
on 6 HIGHs, none in the crypto — P5–P9 above are those findings, fixed in fw 2.69 + this infra revision.
Still open from that gate: P3 (per-device NVS credentials — the only real fix for secrets-in-image), P4
(on-demand AP / per-unit PSK on powered units), TLS-by-default for the broker, scoped InfluxDB tokens,
Telegraf-side `device` tag validation, and a handful of MEDIUM correctness items in the firmware
(`/status` re-reading OneWire from the console task, door-wake buffer timestamp drift, ddmm parse heuristic,
`reportsSinceGps` not NVS-mirrored, Grafana offline-rule lookback). Full table in the session log.

## Independent review (2026-07-14) — outcomes

Two reviewers who did not write the code audited the signed-OTA diff (SSDLC gate).

- **Security review:** the signature gate **holds** — no path installs firmware without a valid signature for the MITM adversary. Verified gate order, hash-covers-flashed-bytes, correct mbedTLS usage, fail-closed `.sig` handling, clean key management.
- **Correctness review — found a CRITICAL bug (now fixed):** the 256-byte `.sig` was fetched over the same HTTP path that has the **3072-byte tail-drop**; since 256 < 3072 the modem may return **nothing**, so **every signed OTA would fail to install**. Fix: the server pads `firmware.bin.sig` to `256 + 4096` and the device reads only the first 256 bytes (same tail-skip trick as the image pieces). Verified: the padded file's first 256 bytes verify OK.
- Also fixed: the manifest "check online" path now sets `ota_size`/`ota_skip`/`ota_md5` (was stale).

### Round 2 — formal `/independent-review` gate (2.50)

A second independent gate (`general-purpose` + `Silas`) **both independently** found a HIGH the
first pass missed: **anti-rollback was cryptographically unenforced** — the signature covered the
image bytes only, so a MITM could relabel any previously-signed image with a higher `ota_ver` and
force a **downgrade**. Fixed in 2.50:

- **HIGH — version binding:** the signature is now over **`version ‖ 0x00 ‖ image`** (server +
  device + `/update`), so a mismatched `ota_ver` fails verification. `verNewer()` + this binding
  together enforce anti-rollback cryptographically.
- **MED — AT-command injection:** `otaUrlSafe()` rejects OTA URLs with CR/LF/quote/control chars
  or bad length before they reach `AT+HTTPPARA`.
- **LOW:** `/update`↔cellular-OTA mutex (`otaUploadActive`); `mbedtls_md_setup/starts` returns now
  checked; `.sig`/hash init failures abort cleanly.
- **Tests:** `infra/scripts/ota.test.ts` pins the tiling invariant, the version-bound sign/verify,
  the downgrade-rejection, and the `verNewer` truth table.

### Round 2 verdict + round 3 (focused re-review) — 2.51, gate PASS

Round 2 confirmed cellular anti-rollback + AT-injection CLOSED but caught a HIGH: the `/update`
version-binding was a **no-op** (`arg("ver")` is empty during the multipart upload callback — the
ESP32 WebServer merges form fields into `_currentArgs` only *after* the callbacks). Fixed in 2.51 by
passing `ver` in the **URL query string** (`/update?ver=…` via the form's `onsubmit`), which a
focused independent re-review **verified against the vendored WebServer parser** is available during
the callback — the `/update` fix is now **CORRECT and FAIL-CLOSED**. Also added: empty-`ver` guard,
`md_setup/starts` return checks on `/update`, a 3-min staleness backstop for `otaUploadActive`, and
an ephemeral-keypair fallback so the signature tests never silently skip.

**Gate PASS (2.51):** no BLOCKER/HIGH. Deployed to the device via WiFi `/update` (a trusted local
channel) as the signing trust anchor.

**MEDIUM + LOWs — FIXED (2.52):** the TOCTOU race is closed with a `portMUX` critical section
(`flashMux`) making each path's claim (check the other's flag + set mine) atomic — the transfer
itself is not held under the lock. `/update` now checks `Update.begin/write` returns (abort + reject
on failure, free the hash ctx), and the version input uses an `id` (no multipart field) so the
`?ver=` query arg can't be shadowed. All addressed as targeted responses to the focused-review
findings; 22/22 tests green.

## Deployment — the trust anchor MUST be UART-flashed

Signed OTA only protects devices *already running* signature-verifying firmware. The
one-time transition from pre-signing firmware (≤2.48) is itself unauthenticated, and
2.48's `.sig` fetch is broken (pre-fix). **Flash 2.49 over UART** to establish the
anchor over a trusted channel. From 2.49 onward, LTE OTA is signed + enforced +
anti-rollback, and 2.49→2.50 is the first real over-the-air signed-update test.

Not in scope without explicit approval: **ESP32 Secure Boot** (eFuse-based,
irreversible, bricking risk). App-level signing below gives most of the benefit
with none of that risk.

## P0 — Signed OTA (image authenticity)

**Design:** RSA-2048 + SHA-256, verified on-device with mbedTLS (already present via
the TLS stack — no new dependency). The signature is transport-independent, so it
holds even though the command channel and the HTTP download are not authenticated.

- **Keys:** `infra/keys/ota_sign_priv.pem` (gitignored, build-host only) +
  `ota_sign_pub.pem`. Public key embedded in `firmware/include/ota_pubkey.h`
  (committed — public half only).
- **Server (`PublishFirmware.ts`):** signs `firmware.bin` →
  `firmware.bin.sig` (`openssl dgst -sha256 -sign`), uploaded alongside the pieces.
- **Device (`performOta`):** streams SHA-256 over the bytes written to flash, fetches
  `<url>.sig`, and verifies the RSA signature against `OTA_PUBKEY_PEM` **before**
  `Update.end`. A bad/absent signature aborts the install. (MD5 is kept as a cheap
  pre-check for accidental corruption.)

Why the sig can ride over plain HTTP: it is unforgeable without the private key, and
the public key is baked into the firmware — a MITM can swap the sig, but can't make
one that verifies. This defeats both the MITM'd-command attack (P0) and, once
`/update` checks it too, the local-flash attack (P1).

## P1 — Gate `/update` on the signature (done, 2.49)

Route the browser/WiFi flash through the same signature check so only signed images
install by any path.

## P2 — Anti-rollback (done, 2.50)

Reject `ota_ver` numerically older than the running `FW_VERSION` unless an explicit
override is set.

## Key management

- **Initial setup (every deployment):** run `bun infra/scripts/GenSigningKey.ts` once,
  from the repo root, before building firmware. It generates the RSA-2048 keypair,
  writes the private key to `infra/keys/ota_sign_priv.pem` (gitignored, build-host
  only), and (re)writes `firmware/include/ota_pubkey.h` (committed). The repo ships a
  **placeholder** `ota_pubkey.h` (the original author's public key) that you MUST
  replace with your own — otherwise you can't sign updates (you don't hold the private
  key) and your fleet would trust someone else's key.
- **Rotation (if the private key is exposed):** delete `infra/keys/ota_sign_priv.pem`,
  re-run `GenSigningKey.ts`, and ship the new `ota_pubkey.h` via a **UART flash** — a
  compromised key can't sign the transition, so OTA can't be trusted for it.
