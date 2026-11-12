// ---------------------------------------------------------------------------
//  tank-monitor-device-model-1-v1.ino - Tank Monitor Device Model-1 v1 (TMM1V1)
//  Wemos/LOLIN D1 mini v2 (ESP8266) + JSN-SR04T ultrasonic sensor, battery powered.
//
//    src/config/globals.h               identity, servers, MQTT, pins, EEPROM layout, power
//    src/config/settings.h              EEPROM settings: Wi-Fi, name, depth, upper/lower gap
//    src/diag/crumbs.h                  RTC breadcrumbs (why did it reset?)
//    src/logging/custom_mqtt_logging.h  LOGE/W/I/D -> USB serial + FirmwareManager console,
//                                       remote restart from the console
//    src/ota/ota_updates.h              OTA updates from FirmwareManager
//    src/net/wifi_connect.h             Wi-Fi, setup portal, power saving, mDNS (watertank.local)
//    src/sensor/tank_sensor.h           distance -> % between the gaps, smoothing, fill detection
//    src/web/dashboard.h                web UI + /api/status /api/config /api/restart
//
//  No extra libraries needed (MQTT over WebSocket client is built in).
//  Wiring: Trig -> D1 (GPIO5), Echo -> D2 (GPIO4), VCC -> 5V, GND -> GND.
//  Log with LOGE / LOGW / LOGI / LOGD, restart only via Diag::restart("reason").
// ---------------------------------------------------------------------------
#include "src/config/globals.h"
#include "src/diag/crumbs.h"
#include "src/logging/custom_mqtt_logging.h"
#include "src/config/settings.h"
#include "src/ota/ota_updates.h"
#include "src/net/wifi_connect.h"
#include "src/sensor/tank_sensor.h"
#include "src/web/dashboard.h"
#include <EEPROM.h>

// =============================================================================
// App hooks
// =============================================================================

// false = an OTA reboot would interrupt something; OTA waits and re-checks.
static bool appCanUpdate() { return true; }

static void appBeforeFlash() {
  LOGI("APP", "preparing for firmware update (level=%.1f%%)", TankSensor::reading().level);
}

// Remote restart from the FirmwareManager console or /api/restart.
static void appBeforeRestart() {
  LOGI("APP", "preparing for restart (level=%.1f%%)", TankSensor::reading().level);
}

// =============================================================================
// Plumbing
// =============================================================================

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();
  ESP.wdtEnable(WDTO_8S);

  Crumbs::begin();             // keep what the previous session left in RTC memory
  EEPROM.begin(EEPROM_SIZE);   // settings + log levels + firmware version
  RemoteLog::init();           // buffer from the first line; sent once MQTT connects

  MqttLogging::setFirmwareVersion(OtaUpdates::currentVersion().c_str());
  MqttLogging::setBeforeRestart(appBeforeRestart);
  OtaUpdates::setCanUpdate(appCanUpdate);
  OtaUpdates::setBeforeFlash(appBeforeFlash);
  Dashboard::setBeforeRestart(appBeforeRestart);

  LOGI("BOOT", "%s (%s) v%s booting - heap=%u core=%s", DEVICE_NAME, MODEL_NUMBER,
       OtaUpdates::currentVersion().c_str(), ESP.getFreeHeap(), ESP.getCoreVersion().c_str());
  LOGI("BOOT", "Wi-Fi MAC %s  <- register this in FirmwareManager -> Devices", WiFi.macAddress().c_str());
  Diag::reportBoot();

  Settings::load();
  TankSensor::begin();

  WifiConnect::begin();  // opens the setup portal (and reboots) if it can't connect
  MqttLogging::loop();   // start remote logging
  OtaUpdates::loop();    // boot check; reboots if an update was installed
  Dashboard::begin();
  TankSensor::loop();    // first reading right away

  LOGI("BOOT", "ready in %lums: http://%s.local level=%.1f%% power-save=%d heap=%u",
       millis(), MDNS_NAME, TankSensor::reading().level, POWER_SAVE_MODE, ESP.getFreeHeap());
  Crumbs::stage("loop");
}

void loop() {
  ESP.wdtFeed();
  WifiConnect::loop();
  Dashboard::loop();
  TankSensor::loop();
  MqttLogging::loop();   // logs, console log levels, remote restart
  OtaUpdates::loop();    // every OTA_CHECK_INTERVAL_MS
  Diag::loop();          // health line + RTC heap sample
  delay(LOOP_IDLE_MS);   // the radio/CPU sleep here (POWER_SAVE_MODE)
}
