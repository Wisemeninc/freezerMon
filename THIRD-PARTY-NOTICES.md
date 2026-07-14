# Third-party notices

freezerMon itself is licensed under the [MIT License](LICENSE). It builds on the
components below, which remain under their own licenses. Nothing here changes the
license of your own code, but note the **LGPL** items when you distribute the
compiled firmware.

## Firmware (compiled into `firmware.bin` and distributed via OTA)

| Component | License | Notes |
|---|---|---|
| [TinyGSM](https://github.com/vshymanskyy/TinyGSM) (lewisxhe A76XX fork) | **LGPL-3.0** | Weak copyleft. You may combine it with MIT-licensed code, but recipients of the firmware binary must be able to relink/replace TinyGSM — satisfied here because the full source is public and rebuildable with PlatformIO. |
| [Arduino-ESP32 core](https://github.com/espressif/arduino-esp32) | LGPL-2.1 + Apache-2.0 | Same weak-copyleft note as above (the framework). |
| [PubSubClient](https://github.com/knolleary/pubsubclient) | MIT | |
| [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) | MIT | |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT | |
| [OneWire](https://github.com/PaulStoffregen/OneWire) | Permissive (grant in source headers; no standalone SPDX license file) | Widely used; review the header terms for strict compliance. |
| Mbed TLS (via ESP-IDF) | Apache-2.0 | Used for the OTA image-signature verification. |

**LGPL compliance:** because this repository publishes complete, buildable source
for the firmware, anyone who receives a `firmware.bin` can rebuild it (optionally
with a modified TinyGSM), which satisfies the LGPL relinking requirement. Keep the
source public/available if you distribute binaries.

## Platform services (run as-is via Docker — not linked into freezerMon's code)

These are used unmodified as network services; their licenses do not propagate to
freezerMon's code.

| Service | License | Notes |
|---|---|---|
| [Grafana](https://github.com/grafana/grafana) 11 | **AGPL-3.0** | Obligation applies only if you **modify** Grafana's source and offer it as a network service. The stock image imposes nothing on you. |
| [Eclipse Mosquitto](https://github.com/eclipse/mosquitto) | EPL-2.0 / EDL-1.0 (dual) | |
| [InfluxDB](https://github.com/influxdata/influxdb) 2.x | MIT | |
| [Telegraf](https://github.com/influxdata/telegraf) | MIT | |
| [Traefik](https://github.com/traefik/traefik) | MIT | |
| [nginx](https://nginx.org/) | BSD-2-Clause | |
