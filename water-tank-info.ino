/*
 * Water Tank Monitoring System — MINIMAL
 * ========================================
 * D1 Mini V2 (ESP8266) + JSN-SR04T Ultrasonic Sensor
 *
 * Stripped-down firmware to prevent random shutoffs:
 *  - WiFi auto-connect with AP fallback & captive portal
 *  - mDNS (watertank.local)
 *  - Simple water level monitoring (3-sample median + spike reject)
 *  - Plain HTML dashboard (auto-refresh every 30s)
 *  - OTA auto-update with semantic versioning
 *  - Remote restart via /api/restart
 *
 * Removed to reduce load:
 *  - Google Sheets logging (HTTPS/BearSSL — huge RAM consumer)
 *  - ArduinoJson library
 *  - Heavy gzipped dashboard with animations
 *  - History buffer (288 entries)
 *  - Fill detection & rate calculation
 *  - EMA filtering & 5-min median pipeline
 *
 * Wiring:
 *  JSN-SR04T Trigger → D1 (GPIO5)
 *  JSN-SR04T Echo    → D2 (GPIO4)
 *  VCC               → 5V
 *  GND               → GND
 */

#include "config_logic.h"
#include "customHttpLogging.h"
#include "dashboard_server.h"
#include "globals.h"
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

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);

  // Hardware watchdog
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);

  printLog("\n\n══════════════════════════════════════");
  printLog("  Water Tank Monitor — MINIMAL v2.0");
  printLog("══════════════════════════════════════");

  // 1. Initialize sensor pins
  initSensor();

  // 2. Initialize EEPROM and load config
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // 3. Attempt WiFi connection
  if (strlen(config.ssid) > 0 && connectWiFi()) {
    // 4. OTA update check
    printLog("[BOOT] Checking for firmware updates...");
    checkAndPerformOTA();

    // 5. Setup mDNS
    setupMDNS();

    // 6. Setup dashboard routes
    setupRoutes();
  } else {
    printLog("[BOOT] Starting Access Point for setup...");
    startAP();
    setupCaptivePortalRoutes();
  }

  // 7. Start web server
  server.begin();

  // 8. Take initial sensor reading
  printLog("[BOOT] Taking initial sensor reading...");
  takeSensorReading();

  printLog("\n══════════════════════════════════════");
  printLog("  Boot complete — system running");
  printLog("  Mode: %s", apMode ? "Access Point" : "WiFi Station");
  printLog("  Free heap: %u bytes", ESP.getFreeHeap());
  printLog("══════════════════════════════════════");
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  ESP.wdtFeed();

  server.handleClient();

  if (!apMode) maintainMDNS();

  unsigned long now = millis();

  // ─── Sensor read every 30 seconds ──────────────────────────
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    takeSensorReading();
  }

  // ─── WiFi keep-alive (check every 30s) ─────────────────────
  if (!apMode) {
    static unsigned long lastWifiCheck = 0;
    static uint8_t wifiFailCount = 0;

    if (now - lastWifiCheck >= 30000) {
      lastWifiCheck = now;

      if (WiFi.status() != WL_CONNECTED) {
        wifiFailCount++;
        printLog("[WIFI] Not connected (fail #%u), reconnecting...", wifiFailCount);
        WiFi.reconnect();

        if (wifiFailCount >= 6) { // 3 minutes
          printLog("[WIFI] Persistent loss — rebooting.");
          delay(200);
          ESP.restart();
        }
      } else {
        wifiFailCount = 0;
      }
    }
  }
}
