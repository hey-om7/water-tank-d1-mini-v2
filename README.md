# Tank Monitor Device Model-1 v1 (TMM1V1)

Battery-powered water tank level monitor on a Wemos/LOLIN **D1 mini v2 (ESP8266)** with a
JSN-SR04T ultrasonic sensor. It shows the level in % at **http://watertank.local**, connects
to FirmwareManager for OTA updates, remote logging and remote restart.

## Files

| File | What it does |
| --- | --- |
| `tank-monitor-device-model-1-v1.ino` | `setup()` / `loop()` plus app hooks |
| `src/config/globals.h` | Identity, version, server URLs, MQTT, pins, EEPROM layout, power saving, smoothing |
| `src/config/settings.h` | EEPROM settings: Wi-Fi, name, tank depth, upper gap, lower gap |
| `src/diag/crumbs.h` | RTC breadcrumbs so every boot logs why the last session ended |
| `src/logging/custom_mqtt_logging.h` | `LOGE/LOGW/LOGI/LOGD` to serial + FirmwareManager console, remote restart |
| `src/ota/ota_updates.h` | OTA updates (primary server first, then fallback) |
| `src/net/wifi_connect.h` | Wi-Fi, setup portal (`TMM1V1-SETUP`), power saving, mDNS, reconnects |
| `src/sensor/tank_sensor.h` | Distance → % between the gaps, spike filter, smoothing, fill detection |
| `src/web/dashboard.h` | Web UI and JSON API |

## Level in %

```
sensor ──┬─
         │ upper gap        → anything closer reads 100 %
 100 % ──┼─
         │ 0–100 % measured linearly in here
   0 % ──┼─
         │ lower gap        → anything lower reads 0 %
bottom ──┴─   tank depth = sensor to bottom
```

Set **tank depth**, **upper gap** and **lower gap** (cm) under *Settings* on the dashboard or
with `POST /api/config`. Readings are smoothed: 3-shot median, spike rejection (5 cm),
then a velocity clamp (20 %/reading), a 0.5 % deadzone and an EMA (α 0.7).

## HTTP API

| Route | |
| --- | --- |
| `GET /` | Dashboard |
| `GET /api/status` | `level`, `rawLevel`, `distance`, `valid`, `filling`, `fullAt`, `emptyAt`, `rssi`, `mqtt`, `fw`, `mac`, ... |
| `GET /api/config` | `name`, `tankDepth`, `upperGap`, `lowerGap`, `ssid` |
| `POST /api/config` | form fields `name`, `tankDepth`, `upperGap`, `lowerGap` |
| `GET` or `POST /api/restart` | Reboots after replying |

`/api/*` sends CORS headers, so a browser app on another origin can call it.

## First flash (USB)

1. Board: **LOLIN(WEMOS) D1 R2 & mini**, Flash Size **4MB (FS:1MB OTA:~1019KB)**.
2. No extra libraries are needed (the MQTT client is built in).
3. Flash, open Serial Monitor at 115200 baud, copy the printed **Wi-Fi MAC**.
4. FirmwareManager → **Devices → Add device**, enter the MAC, model `TMM1V1`.
5. Join the `TMM1V1-SETUP` Wi-Fi from your phone and pick your network.
   The portal also opens for 3 minutes whenever the saved network can't be reached at boot.

## Power

`POWER_SAVE_MODE` in `globals.h`: `1` modem sleep (default), `0` always on, `2` light sleep (drops the MQTT session on the D1 mini; not recommended). Power saving starts only once the MQTT session is up.
The device stays connected so the web page, MQTT and remote restart keep working, so it can't
deep-sleep. To cut sensor current between readings, switch its supply from a GPIO and set
`SENSOR_POWER_PIN`.

## Shipping updates

1. Bump `FW_VERSION` in `src/config/globals.h`.
2. **Sketch → Export Compiled Binary**.
3. FirmwareManager → **Firmware → Tank Monitor Device Model-1 v1 → New build**, same version.

## Remote restart

FirmwareManager → **Devices → (device) → Overview → Restart device** (over MQTT), or
`/api/restart` on the device.

## Security

`globals.h` contains the shared MQTT password. Keep this project out of public repositories.
