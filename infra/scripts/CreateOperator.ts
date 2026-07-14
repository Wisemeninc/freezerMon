#!/usr/bin/env bun
/**
 * Creates the read-only "operator" login via Grafana's admin API.
 * Run once after `docker compose up -d`:
 *
 *   cd infra && set -a && source .env && set +a && bun scripts/CreateOperator.ts
 */
const base = process.env.GRAFANA_URL ?? "http://localhost:3000";
const adminPass = process.env.GRAFANA_ADMIN_PASSWORD;
const operatorPass = process.env.GRAFANA_OPERATOR_PASSWORD;

if (!adminPass || !operatorPass) {
  console.error("Set GRAFANA_ADMIN_PASSWORD and GRAFANA_OPERATOR_PASSWORD (source infra/.env first).");
  process.exit(1);
}

const res = await fetch(`${base}/api/admin/users`, {
  method: "POST",
  headers: {
    "Content-Type": "application/json",
    Authorization: "Basic " + btoa(`admin:${adminPass}`),
  },
  body: JSON.stringify({ name: "Operator", login: "operator", password: operatorPass }),
});

const body = (await res.json()) as { id?: number; message?: string };
if (!res.ok) {
  console.error(`Grafana API ${res.status}: ${body.message ?? JSON.stringify(body)}`);
  process.exit(1);
}
console.log(`operator user created (id ${body.id}) — role: Viewer (read-only)`);
