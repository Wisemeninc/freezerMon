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
| **P3** | **Secrets compiled into the binary** (MQTT + AP passwords). Extractable via flash dump / physical access. Note: the committed *example* `DEBUG_AP_PASSWORD` is weak — must be unique + high-entropy per unit. | Open — per-device creds |
| P4 | Info disclosure on the debug console (`/lte`, `/gps`, `/log`) — behind WPA2. | Low |

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

## P1 — Gate `/update` on the signature (planned)

Route the browser/WiFi flash through the same signature check so only signed images
install by any path.

## P2 — Anti-rollback (planned)

Reject `ota_ver` numerically older than the running `FW_VERSION` unless an explicit
override is set.

## Key rotation

If the private key is ever exposed: generate a new keypair, update
`ota_pubkey.h`, and ship it via a **UART flash** (a compromised key can't sign the
transition, so OTA can't be trusted for it).
