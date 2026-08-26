#!/usr/bin/env bun
/**
 * Push an OTA update to a device over LTE.
 *
 *   bun infra/scripts/PublishFirmware.ts <version> [device]
 *
 * Uploads firmware/.pio/build/t-a7608-tls/firmware.bin to the server's OTA
 * path and sets the retained command on freezer/<device>/cmd. The device
 * compares ota_ver against its own FW_VERSION on its next report and pulls
 * the image when they differ (battery permitting). Build first:
 *   docker/pio: pio run -e t-a7608-tls   (FW_VERSION in main.cpp must equal <version>)
 */
import { $ } from "bun";
import { existsSync, readFileSync, writeFileSync, mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createHash, sign as cryptoSign } from "node:crypto";

const SERVER = process.env.FREEZERMON_SERVER ?? "root@your-server.example.com";
const HOST = process.env.FREEZERMON_HOST ?? "freezer.example.com";
const BIN = "firmware/.pio/build/t-a7608-tls/firmware.bin";

const ver = process.argv[2];
const device = process.argv[3] ?? "cooler-01";
if (!ver) {
  console.error("usage: bun infra/scripts/PublishFirmware.ts <version> [device]");
  process.exit(1);
}
// Both values end up in a remote command line and in an MQTT topic. Validate against
// the firmware's own contracts (verNewer's "%d.%d" parse, deviceNameValid's charset)
// rather than trusting argv — this script runs as root on the telemetry host.
if (!/^\d{1,3}\.\d{1,3}$/.test(ver)) {
  console.error(`version must be <major>.<minor> digits only (got ${JSON.stringify(ver)})`);
  process.exit(1);
}
if (!/^[a-z0-9-]{1,21}$/.test(device)) {
  console.error(`device must match [a-z0-9-]{1,21} (got ${JSON.stringify(device)})`);
  process.exit(1);
}
if (!existsSync(BIN)) {
  console.error(`${BIN} not found — build with: pio run -e t-a7608-tls`);
  process.exit(1);
}

const tokenLine = (await $`ssh ${SERVER} grep ^OTA_PATH_TOKEN= /opt/freezermon/.env`.text()).trim();
const token = tokenLine.slice(tokenLine.indexOf("=") + 1);
if (!/^[A-Za-z0-9_-]{16,}$/.test(token)) {
  console.error("OTA_PATH_TOKEN missing or malformed in server /opt/freezermon/.env (expect >=16 chars of [A-Za-z0-9_-])");
  process.exit(1);
}

await $`ssh ${SERVER} mkdir -p /opt/freezermon/ota/${token}`;
// The unsplit image is NOT published: it holds every compiled-in credential and the
// token-path is served over plain HTTP. The device only needs the pieces + .sig;
// the WiFi /update form is fed straight from the build host. Remove a stale copy too.
await $`ssh ${SERVER} rm -f /opt/freezermon/ota/${token}/firmware.bin`;

// The A7608 can't pull the whole image in one request (its HTTP cache can't hold
// a big response and won't cache 206 partials), so we split it into small plain
// 200 GETs. The remaining trap: the modem DROPS the last ~3072 bytes of every
// cached response — it reports the full Content-Length but only caches
// length-3072, then ERRORs. So pieces OVERLAP: each covers PIECE bytes but
// advances by STEP = PIECE-OVERLAP, and the device reads only the deliverable
// prefix (plen - OVERLAP) of each, which tiles the image with no gap. The short
// final piece is zero-padded so the drop lands in padding, not real data. The
// per-piece skip travels to the device as `ota_skip`, so OVERLAP is tunable here
// with no reflash. See docs/OTA-INVESTIGATION.md.
const PIECE = 32768;              // bytes fetched per piece (well within the cache)
const OVERLAP = 4096;             // > the ~3072 drop; also the device's per-piece skip
const STEP = PIECE - OVERLAP;     // real bytes consumed per piece (28 KB)
const data = readFileSync(BIN);
const total = data.length;
const md5 = createHash("md5").update(data).digest("hex");  // cheap corruption pre-check
const nPieces = Math.ceil(total / STEP);

// Sign the image so the device installs it ONLY if it verifies against the public
// key baked into firmware (ota_pubkey.h) — authenticity independent of the (MITM-able)
// command channel and HTTP download. RSA-2048 + SHA-256, matching mbedtls_pk_verify
// on-device. The 256-byte sig ships as firmware.bin.sig (fetched at <url>.sig).
const SIGN_KEY = "infra/keys/ota_sign_priv.pem";
if (!existsSync(SIGN_KEY)) {
  console.error(`signing key ${SIGN_KEY} not found — generate it (see docs/HARDENING.md)`);
  process.exit(1);
}
// Sign (version ‖ 0x00 ‖ image), NOT the image alone — this binds the version into
// the signature so a MITM can't relabel an old signed image with a higher ota_ver to
// force a downgrade (anti-rollback is only as strong as verNewer + this binding).
const signedMsg = Buffer.concat([Buffer.from(ver, "utf8"), Buffer.from([0]), data]);
const sig = cryptoSign("sha256", signedMsg, readFileSync(SIGN_KEY, "utf8"));  // PKCS#1 v1.5, 256 bytes
const dir = mkdtempSync(join(tmpdir(), "ota-"));
for (let i = 0; i < nPieces; i++) {
  const start = i * STEP;
  let piece = data.subarray(start, Math.min(start + PIECE, total)); // up to PIECE real bytes
  const realToRead = Math.min(STEP, total - start);  // real bytes the device will read
  const needLen = realToRead + OVERLAP;              // keep the drop past the real data
  if (piece.length < needLen) {
    const padded = Buffer.alloc(needLen);            // zero-filled tail; device never reads it
    piece.copy(padded, 0);
    piece = padded;
  }
  writeFileSync(join(dir, `firmware.bin.${String(i).padStart(3, "0")}`), piece);
}
// Pad the 256-byte sig past the modem's ~3072-byte cached-response tail-drop: a bare
// 256-byte file can't be HTTPREAD out at all. The device reads only the first 256.
const sigPadded = Buffer.alloc(256 + OVERLAP);
sig.copy(sigPadded, 0);
writeFileSync(join(dir, "firmware.bin.sig"), sigPadded);   // fetched by the device at <url>.sig
await $`scp -q ${dir}/firmware.bin.* ${SERVER}:/opt/freezermon/ota/${token}/`;

// Plain HTTP; token path is the access control; ESP32 Update validates the image.
const url = `http://${HOST}/fw/${token}/firmware.bin`;

// The device fetches <url>.000, .001, … reading (plen - ota_skip) real bytes each
// until ota_size total (the unsplit firmware.bin is deliberately not served).
const cmd = JSON.stringify({ ota_url: url, ota_ver: ver, ota_size: total, ota_skip: OVERLAP, ota_md5: md5 });
const manifest = JSON.stringify({ ota_ver: ver, ota_url: url, ota_size: total, ota_skip: OVERLAP, ota_md5: md5 });
await $`ssh ${SERVER} tee /opt/freezermon/ota/${token}/manifest.json < ${new Response(manifest + "\n")}`.quiet();
// Retained command, published as the `publisher` identity (ACL: cmd topics only).
// The remote command line contains exactly one caller-influenced value — the topic —
// and it was validated above against [a-z0-9-]{1,21}; the payload travels on stdin.
const topic = `freezer/${device}/cmd`;
const remote =
  "set -a; . /opt/freezermon/.env; set +a; " +
  "exec docker exec -i -e PUB_USER=\"${PUBLISHER_MQTT_USER:-publisher}\" -e PUB_PASS=\"$PUBLISHER_MQTT_PASSWORD\" " +
  "freezermon-mosquitto-1 sh -c 'exec mosquitto_pub -u \"$PUB_USER\" -P \"$PUB_PASS\" -r -t \"$0\" -s' " +
  "'" + topic + "'";
await $`ssh ${SERVER} ${remote} < ${new Response(cmd + "\n")}`.quiet();

console.log(`OTA published: ${device} → v${ver}  (${total} bytes, ${nPieces} pieces of ${PIECE}, ${STEP}B step, ${OVERLAP}B overlap)`);
console.log(`  image: ${url}`);
console.log(`  the retained command applies on the device's next report cycle`);
