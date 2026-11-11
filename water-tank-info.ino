/*
 * Water Tank Monitoring System
 * ========================================
 * D1 Mini V2 (ESP8266) + JSN-SR04T Ultrasonic Sensor
 *
 *  - WiFi auto-connect with AP fallback & captive portal
 *  - mDNS (watertank.local)
 *  - Water level monitoring (3-sample median + spike reject, every 30 s)
 *  - Plain HTML dashboard (auto-refresh every 30s) + /config settings page
 *  - On-device level smoothing: velocity clamp -> deadzone -> EMA
 *  - Fill detection (rise over the last 50 min)
 *  - FirmwareManager integration:
 *      * OTA updates (boot + every 6 h)
 *      * Remote logging over MQTT; levels set in Devices -> Logging
 *      * Remote restart from the console
 *  - Reset diagnostics: every boot logs WHY the previous session ended
 *    (power loss, crash, watchdog, deliberate restart + last stage + heap)
 *
 * Logging: use LOGE / LOGW / LOGI / LOGD(tag, fmt, ...), never Serial.print.
 * Every deliberate restart goes through Diag::restart("reason").
 *
 * Required library: "WebSockets" by Markus Sattler (Library Manager).
 *
 * Wiring:
 *  JSN-SR04T Trigger → D1 (GPIO5)
 *  JSN-SR04T Echo    → D2 (GPIO4)
 *  VCC               → 5V
 *  GND               → GND
 */

#include "globals.h"
#include "diagnostics.h"
#include "custom_mqtt_logging.h"
#include "config_logic.h"
#include "dashboard_server.h"
#include "ota_update_logic.h"
#include "sensor_logic.h"
#include "wifi_logic.h"

// ─── Global Variable Definitions ─────────────────────────────
ESP8266WebServer server(80);
Config config;
bool apMode = false;

// Sensor state
float currentDistance = -1;
float currentLevel = 0;
unsigned long lastSensorRead = 0;

// Sensor filter state
float lastValidDistance = -1;
int spikeCount = 0;

// Fill detection state
bool isFilling = false;
float fillRate = 0;

// ─── FirmwareManager hooks ───────────────────────────────────
// Runs right before a firmware download starts.
static void appBeforeFlash() {
  LOGI("APP", "preparing for firmware update (level=%.1f%%)", currentLevel);
}

// Runs right before a restart requested from the FirmwareManager console.
static void appBeforeRestart() {
  LOGI("APP", "preparing for remote restart (level=%.1f%%)", currentLevel);
}

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println();

  // Hardware watchdog
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);

  // Read what the previous session left in RTC memory before anything else.
  Crumbs::begin();

  // EEPROM holds config + log levels; needed before the first log line.
  EEPROM.begin(EEPROM_SIZE);
  RemoteLog::init();

  MqttLogging::setFirmwareVersion(OtaUpdates::currentVersion().c_str());
  MqttLogging::setBeforeRestart(appBeforeRestart);
  OtaUpdates::setBeforeFlash(appBeforeFlash);

  LOGI("BOOT", "%s (%s) v%s booting - heap=%u core=%s sdk=%s", DEVICE_NAME, MODEL_NUMBER,
       OtaUpdates::currentVersion().c_str(), ESP.getFreeHeap(), ESP.getCoreVersion().c_str(), ESP.getSdkVersion());
  LOGI("BOOT", "Wi-Fi MAC %s  <- register this in FirmwareManager -> Devices", WiFi.macAddress().c_str());
  LOGI("BOOT", "reset info: %s", ESP.getResetInfo().c_str());
  Diag::reportBoot();

  // 1. Initialize sensor pins
  initSensor();

  // 2. Load config
  loadConfig();

  // 3. Attempt WiFi connection
  setupWifiEvents();
  if (strlen(config.ssid) > 0 && connectWiFi()) {
    // 4. Start remote logging (boot lines buffered so far are sent)
    MqttLogging::loop();

    // 5. OTA update check (reboots if an update was installed)
    OtaUpdates::loop();

    // 6. Setup mDNS
    setupMDNS();

    // 7. Setup dashboard routes
    setupRoutes();
  } else {
    LOGW("BOOT", "no Wi-Fi (%s) - starting access point for setup",
         strlen(config.ssid) ? "connection failed" : "not configured");
    startAP();
    setupCaptivePortalRoutes();
  }

  // 8. Start web server
  server.begin();

  // 9. Take initial sensor reading
  takeSensorReading();
  LOGI("SENSOR", "initial reading: %.1fcm level=%.1f%%", currentDistance, currentLevel);

  LOGI("BOOT", "boot complete in %lums - mode=%s heap=%u maxBlock=%u", millis(),
       apMode ? "access point" : "station", ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
  Crumbs::stage("loop");
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  ESP.wdtFeed();
  Crumbs::stage("loop");

  server.handleClient();

  if (!apMode) {
    maintainMDNS();
    maintainArp();
  }

  unsigned long now = millis();

  // ─── Sensor read every 30 seconds ──────────────────────────
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    takeSensorReading();
  }

  if (!apMode) {
    // ─── FirmwareManager: logging, remote restart, OTA ─────────
    MqttLogging::loop();
    OtaUpdates::loop();

    // ─── WiFi keep-alive (check every 30s) ─────────────────────
    static unsigned long lastWifiCheck = 0;
    static uint8_t wifiFailCount = 0;

    if (now - lastWifiCheck >= 30000) {
      lastWifiCheck = now;

      if (WiFi.status() != WL_CONNECTED) {
        wifiFailCount++;
        LOGW("WIFI", "not connected (check #%u, status=%d), reconnecting", wifiFailCount,
             static_cast<int>(WiFi.status()));
        WiFi.reconnect();

        if (wifiFailCount >= 6) { // 3 minutes
          Diag::restart("Wi-Fi lost for 3 minutes");
        }
      } else {
        if (wifiFailCount) LOGI("WIFI", "link OK again after %u failed checks", wifiFailCount);
        wifiFailCount = 0;
      }
    }
  }

  // ─── Health line + RTC heap sample ─────────────────────────
  Diag::loop();
}
