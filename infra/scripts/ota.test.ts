// Off-device regression tests for the OTA security-critical logic.
//   bun test infra/scripts/ota.test.ts
import { test, expect, describe } from "bun:test";
import { sign, verify, randomBytes, generateKeyPairSync } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";

// ---- constants mirrored from PublishFirmware.ts / firmware performOta() ----
const PIECE = 32768, OVERLAP = 4096, STEP = PIECE - OVERLAP;

// server split (PublishFirmware.ts) -> device read (performOta) -> reassembled image
function roundTrip(data: Buffer): Buffer {
  const total = data.length;
  const out: Buffer[] = [];
  let done = 0;
  const nPieces = Math.ceil(total / STEP);
  for (let i = 0; i < nPieces && done < total; i++) {
    const start = i * STEP;
    let piece: Buffer = Buffer.from(data.subarray(start, Math.min(start + PIECE, total)));
    const realToRead = Math.min(STEP, total - start);
    const needLen = realToRead + OVERLAP;
    if (piece.length < needLen) { const p = Buffer.alloc(needLen); piece.copy(p, 0); piece = p; }
    // device: want = min(plen - OVERLAP, remaining), read from offset 0
    let want = piece.length - OVERLAP;
    if (total - done < want) want = total - done;
    out.push(Buffer.from(piece.subarray(0, want)));
    done += want;
  }
  return Buffer.concat(out);
}

describe("piece tiling invariant (split -> read -> reassemble == original)", () => {
  const sizes = [1, 1024, STEP - 1, STEP, STEP + 1, PIECE, 907696, 3 * STEP, 3 * STEP + 7, 1_000_003];
  for (const n of sizes) {
    test(`size ${n}`, () => {
      const data = randomBytes(n);
      const recon = roundTrip(data);
      expect(recon.length).toBe(n);
      expect(recon.equals(data)).toBe(true);
    });
  }
});

// verNewer() replica of the C firmware logic (sscanf "%d.%d", compare major then minor)
function verNewer(cand: string, cur: string): boolean {
  const c = cand.match(/^(\d+)(?:\.(\d+))?/); if (!c) return false;
  const r = cur.match(/^(\d+)(?:\.(\d+))?/);  if (!r) return false;
  const c1 = +c[1], c2 = +(c[2] ?? 0), r1 = +r[1], r2 = +(r[2] ?? 0);
  return c1 !== r1 ? c1 > r1 : c2 > r2;
}

describe("verNewer anti-rollback truth table", () => {
  const cases: [string, string, boolean][] = [
    ["2.50", "2.49", true], ["2.49", "2.49", false], ["2.48", "2.49", false],
    ["3.0", "2.99", true],  ["2.100", "2.99", true], ["2.5", "2.49", false], // integer-tuple: 5 < 49
    ["10.0", "9.99", true], ["garbage", "2.49", false], ["", "2.49", false],
  ];
  for (const [c, r, want] of cases) {
    test(`verNewer(${JSON.stringify(c)}, ${r}) == ${want}`, () => expect(verNewer(c, r)).toBe(want));
  }
});

const KEY = "infra/keys/ota_sign_priv.pem", PUB = "infra/keys/ota_sign_pub.pem";
// Use the real keys if present, else an ephemeral pair — so these security invariants
// ALWAYS run (they must never silently skip on a clean checkout / CI).
const keys = existsSync(KEY) && existsSync(PUB)
  ? { privateKey: readFileSync(KEY, "utf8"), publicKey: readFileSync(PUB, "utf8") }
  : generateKeyPairSync("rsa", { modulusLength: 2048,
      publicKeyEncoding: { type: "spki", format: "pem" },
      privateKeyEncoding: { type: "pkcs8", format: "pem" } });
describe("version-bound signature (sign ver‖0‖image)", () => {
  const priv = keys.privateKey, pub = keys.publicKey;
  const image = randomBytes(40000);
  const ver = "2.50";
  const signedMsg = Buffer.concat([Buffer.from(ver, "utf8"), Buffer.from([0]), image]);
  const sig = sign("sha256", signedMsg, priv);
  const padded = Buffer.alloc(256 + OVERLAP); sig.copy(padded, 0);
  const first256 = padded.subarray(0, 256);   // what the device reads

  test("padded sig's first 256 bytes verify over ver‖0‖image", () => {
    expect(verify("sha256", signedMsg, pub, first256)).toBe(true);
  });
  test("DOWNGRADE BLOCKED: same image relabelled with a higher version fails", () => {
    const attack = Buffer.concat([Buffer.from("9.99", "utf8"), Buffer.from([0]), image]);
    expect(verify("sha256", attack, pub, first256)).toBe(false);
  });
  test("TAMPER BLOCKED: one flipped image byte fails", () => {
    const t = Buffer.from(image); t[1000] ^= 0xff;
    const tMsg = Buffer.concat([Buffer.from(ver, "utf8"), Buffer.from([0]), t]);
    expect(verify("sha256", tMsg, pub, first256)).toBe(false);
  });
});
