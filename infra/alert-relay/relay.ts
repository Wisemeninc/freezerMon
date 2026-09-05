#!/usr/bin/env bun
/**
 * Grafana -> Signal relay. Grafana's webhook contact point POSTs its alert JSON here;
 * this turns it into a short human message and hands it to signal-cli-rest-api
 * (bbernhard/signal-cli-rest-api) on the internal network. No inbound auth: the
 * port is never published outside the compose network.
 *
 *   SIGNAL_URL         http://signal:8080 (default)
 *   SIGNAL_NUMBER      the Signal account the messages are sent FROM (+45...)
 *   SIGNAL_RECIPIENTS  comma-separated phone numbers and/or group ids to notify
 */
const SIGNAL_URL = process.env.SIGNAL_URL ?? "http://signal:8080";
const NUMBER = process.env.SIGNAL_NUMBER ?? "";
const RECIPIENTS = (process.env.SIGNAL_RECIPIENTS ?? "").split(",").map((s) => s.trim()).filter(Boolean);

async function send(message: string): Promise<boolean> {
  if (!NUMBER || RECIPIENTS.length === 0) { console.error("SIGNAL_NUMBER / SIGNAL_RECIPIENTS not set"); return false; }
  // Whatever posted this (Grafana, or anything else on the compose network) does not get
  // to push control characters or a novel to a phone: printable text, capped length.
  message = message.replace(/[^\P{C}\n]/gu, "").slice(0, 1500);
  const r = await fetch(`${SIGNAL_URL}/v2/send`, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message, number: NUMBER, recipients: RECIPIENTS }),
  });
  if (!r.ok) console.error("signal send failed:", r.status, await r.text());
  return r.ok;
}

type GrafanaAlert = { status: string; labels?: Record<string, string>; annotations?: Record<string, string>; valueString?: string };

/**
 * A unit that has not reported for this long gets no messages at all — no reminders,
 * no "OK again". Every provisioned rule carries a `last_report_age_s` annotation
 * (seconds since the unit's newest reading, re-rendered each evaluation). The offline
 * rule already said "silent" once at 15 min; after a day nobody can act on a stale
 * temperature or battery reading, and an "OK again" produced by anything other than a
 * fresh reading would be a lie. 0 disables the mute. A rule firing with NoData has no
 * reading in its 30-day lookback at all — older than any sensible threshold, muted too.
 */
const MUTE_AFTER_SILENT_S = Number(process.env.MUTE_AFTER_SILENT_S ?? "86400");
export function silentTooLong(a: GrafanaAlert, threshold = MUTE_AFTER_SILENT_S): boolean {
  if (!(threshold > 0)) return false;
  if (a.annotations?.grafana_state_reason === "NoData") return true;
  const age = Number(a.annotations?.last_report_age_s);
  return Number.isFinite(age) && age > threshold;
}

export function format(body: { alerts?: GrafanaAlert[]; title?: string; status?: string }): string {
  const lines: string[] = [];
  for (const a of body.alerts ?? []) {
    const firing = a.status === "firing";
    const name = a.labels?.alertname ?? "alert";
    const dev = a.labels?.device ? ` — ${a.labels.device}` : "";
    const text = a.annotations?.summary ?? a.annotations?.description ?? "";
    lines.push(`${firing ? "🔴 ALARM" : "✅ OK again"}: ${name}${dev}${text ? `\n${text}` : ""}`);
  }
  return lines.join("\n\n") || `freezerMon: ${body.title ?? body.status ?? "alert"}`;
}

/** Splits a Grafana payload into what goes to Signal and what is muted (with a reason). */
export function triage(body: { alerts?: GrafanaAlert[] }): { keep: GrafanaAlert[]; muted: string[] } {
  const keep: GrafanaAlert[] = [], muted: string[] = [];
  for (const a of body.alerts ?? []) {
    if (silentTooLong(a)) muted.push(`${a.labels?.alertname ?? "alert"}/${a.labels?.device ?? "-"} ${a.status} ${a.annotations?.grafana_state_reason === "NoData" ? "no data in lookback" : `age=${a.annotations?.last_report_age_s}s`}`);
    else keep.push(a);
  }
  return { keep, muted };
}

if (import.meta.main) Bun.serve({
  port: 8091,
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/health") return new Response("ok");
    if (url.pathname === "/test") { const ok = await send("freezerMon: test message — alerts are wired to Signal."); return new Response(ok ? "sent\n" : "failed (see logs)\n", { status: ok ? 200 : 502 }); }
    if (url.pathname === "/grafana" && req.method === "POST") {
      const len = Number(req.headers.get("content-length") ?? "0");
      if (len > 65536) return new Response("too large", { status: 413 });
      let body: any; try { body = JSON.parse((await req.text()).slice(0, 65536)); } catch { return new Response("bad json", { status: 400 }); }
      if (process.env.RELAY_DEBUG) console.log("grafana payload:", JSON.stringify(body).slice(0, 2000));
      const { keep, muted } = triage(body ?? {});
      if (muted.length) console.log(`muted (unit silent > ${MUTE_AFTER_SILENT_S}s):`, muted.join("; "));
      if (muted.length && keep.length === 0) return new Response("muted", { status: 200 });
      const ok = await send(format({ ...body, alerts: keep }));
      return new Response(ok ? "sent" : "signal send failed", { status: ok ? 200 : 502 });
    }
    return new Response("not found", { status: 404 });
  },
});
if (import.meta.main) console.log("alert-relay listening on :8091 ->", SIGNAL_URL, "recipients:", RECIPIENTS.length);
