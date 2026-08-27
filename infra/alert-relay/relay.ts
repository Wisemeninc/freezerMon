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
  const r = await fetch(`${SIGNAL_URL}/v2/send`, {
    method: "POST", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message, number: NUMBER, recipients: RECIPIENTS }),
  });
  if (!r.ok) console.error("signal send failed:", r.status, await r.text());
  return r.ok;
}

type GrafanaAlert = { status: string; labels?: Record<string, string>; annotations?: Record<string, string>; valueString?: string };
function format(body: { alerts?: GrafanaAlert[]; title?: string; status?: string }): string {
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

Bun.serve({
  port: 8091,
  async fetch(req) {
    const url = new URL(req.url);
    if (url.pathname === "/health") return new Response("ok");
    if (url.pathname === "/test") return new Response((await send("freezerMon: test message — alerts are wired to Signal.")) ? "sent\n" : "failed (see logs)\n", { status: 200 });
    if (url.pathname === "/grafana" && req.method === "POST") {
      let body: any; try { body = await req.json(); } catch { return new Response("bad json", { status: 400 }); }
      if (process.env.RELAY_DEBUG) console.log("grafana payload:", JSON.stringify(body).slice(0, 4000));
      const ok = await send(format(body));
      return new Response(ok ? "sent" : "signal send failed", { status: ok ? 200 : 502 });
    }
    return new Response("not found", { status: 404 });
  },
});
console.log("alert-relay listening on :8091 ->", SIGNAL_URL, "recipients:", RECIPIENTS.length);
