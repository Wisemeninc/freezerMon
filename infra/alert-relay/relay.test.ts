import { describe, expect, test } from "bun:test";
import { format, silentTooLong, triage } from "./relay";

const alert = (over: Partial<{ status: string; device: string; age: string; reason: string; name: string }> = {}) => ({
  status: over.status ?? "firing",
  labels: { alertname: over.name ?? "Cabinet temperature high", device: over.device ?? "cooler-01" },
  annotations: {
    summary: "Cabinet 9.1 °C — limit 8 °C (cooler-01)",
    ...(over.age !== undefined ? { last_report_age_s: over.age } : {}),
    ...(over.reason ? { grafana_state_reason: over.reason } : {}),
  },
});

describe("silentTooLong", () => {
  test("fresh reading passes", () => expect(silentTooLong(alert({ age: "600" }), 86400)).toBe(false));
  test("exactly the threshold passes", () => expect(silentTooLong(alert({ age: "86400" }), 86400)).toBe(false));
  test("older than a day is muted", () => expect(silentTooLong(alert({ age: "90000" }), 86400)).toBe(true));
  test("resolved messages are muted the same way", () => expect(silentTooLong(alert({ status: "resolved", age: "90000" }), 86400)).toBe(true));
  test("missing or unrenderable annotation passes (fail open)", () => {
    expect(silentTooLong(alert(), 86400)).toBe(false);
    expect(silentTooLong(alert({ age: "" }), 86400)).toBe(false);
    expect(silentTooLong(alert({ age: "<no value>" }), 86400)).toBe(false);
  });
  test("NoData state is muted", () => expect(silentTooLong(alert({ reason: "NoData" }), 86400)).toBe(true));
  test("threshold 0 disables the mute", () => {
    expect(silentTooLong(alert({ age: "9999999" }), 0)).toBe(false);
    expect(silentTooLong(alert({ reason: "NoData" }), 0)).toBe(false);
  });
});

describe("triage", () => {
  test("keeps the live unit, mutes the silent one", () => {
    const body = { alerts: [alert({ device: "cooler-01", age: "120" }), alert({ device: "cooler-02", age: "200000" })] };
    const { keep, muted } = triage(body);
    expect(keep.map((a) => a.labels.device)).toEqual(["cooler-01"]);
    expect(muted).toHaveLength(1);
    expect(muted[0]).toContain("cooler-02");
    expect(format({ ...body, alerts: keep })).toContain("cooler-01");
    expect(format({ ...body, alerts: keep })).not.toContain("cooler-02");
  });
  test("empty payload is untouched", () => expect(triage({})).toEqual({ keep: [], muted: [] }));
});
