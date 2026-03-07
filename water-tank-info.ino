/*
 * Water Tank Monitoring System
 * =============================
 * D1 Mini V2 (ESP8266) + JSN-SR04T Ultrasonic Sensor
 *
 * Features:
 *  - WiFi auto-connect with AP fallback & captive portal
 *  - mDNS (watertank.local)
 *  - Stabilized water level monitoring (median + EMA filtering)
 *  - Fill detection with rate & ETA calculation
 *  - OTA auto-update with semantic versioning
 *  - Google Sheets logging (every 5 min)
 *  - Modern web dashboard with smooth animations
 *  - EEPROM configuration persistence
 *  - Factory reset & remote restart
 *
 * Wiring:
 *  JSN-SR04T Trigger → D1 (GPIO5)
 *  JSN-SR04T Echo    → D2 (GPIO4)
 *  VCC               → 5V
 *  GND               → GND
 *
 * Sensor Pipeline:
 *  Every 2s: read 7 samples → median → spike reject → EMA smooth → buffer
 *  Every 30s: average buffer → publish stable distance & level to dashboard
 *
 * Modular Architecture:
 *  globals.h          → Constants, types, extern declarations
 *  config_logic.h     → EEPROM config read/write, factory reset
 *  wifi_logic.h       → WiFi STA/AP/mDNS management
 *  ota_update_logic.h → OTA version check & firmware update
 *  sensor_logic.h     → Ultrasonic sensor, filtering & fill detection
 *  dashboard_server.h → HTTP server, API routes, captive portal
 *  dashboard.h        → Gzipped dashboard HTML
 */

#include "config_logic.h"
#include "dashboard_server.h"
#include "globals.h"
#include "ota_update_logic.h"
#include "sensor_logic.h"
#include "wifi_logic.h"

// ─── Global Variable Definitions ─────────────────────────────
// (Declared as extern in globals.h)
ESP8266WebServer server(80);
Config config;
bool apMode = false;

// Sensor state
float currentDistance = 0;
float currentLevel = 0; // 0-100%
unsigned long lastSensorRead = 0;

// Sensor filter state
float filterBuffer[FILTER_BUFFER_SIZE];
int filterBufIdx = 0;
int filterBufCount = 0;
float lastValidDistance = -1;
float emaDistance = -1;
unsigned long lastInternalRead = 0;

// Fill detection state
float levelHistory[FILL_WINDOW];
int levelHistIdx = 0;
bool isFilling = false;
float fillRate = 0; // %/min
float fillEta = 0;  // minutes to full
unsigned long lastFillCheck = 0;

// History buffer (circular)
HistoryEntry historyBuf[HISTORY_SIZE];
int historyHead = 0;
int historyCount = 0;

// Logging
unsigned long lastLogTime = 0;

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n\n══════════════════════════════════════");
  Serial.println("  Water Tank Monitor — Starting Up");
  Serial.println("══════════════════════════════════════");

  // 1. Initialize sensor pins & filter
  initSensor();
  initFillDetection();
  initFilter();

  // 2. Initialize EEPROM and load config
  EEPROM.begin(EEPROM_SIZE);
  loadConfig();

  // 3. Attempt WiFi connection
  if (strlen(config.ssid) > 0 && connectWiFi()) {
    // 4. Run OTA update check (before normal operation)
    Serial.println("\n[BOOT] Checking for firmware updates...");
    checkAndPerformOTA();

    // 5. Setup mDNS
    setupMDNS();

    // 6. Setup dashboard routes
    setupRoutes();
  } else {
    // No saved WiFi or connection failed → AP mode
    Serial.println("[BOOT] No WiFi credentials or connection failed");
    Serial.println("[BOOT] Starting Access Point for setup...");
    startAP();
    setupCaptivePortalRoutes();
  }

  // 7. Start web server
  server.begin();

  // 8. Take initial sensor reading
  Serial.println("[BOOT] Taking initial sensor readings...");
  for (int i = 0; i < 5; i++) {
    internalSensorRead();
    delay(500);
  }
  publishStableReading();

  Serial.println("\n══════════════════════════════════════");
  Serial.println("  Boot complete — system running");
  Serial.printf("  Mode: %s\n", apMode ? "Access Point" : "WiFi Station");
  Serial.printf("  Sensor: read every %ds, publish every %ds\n",
                SENSOR_READ_INTERVAL / 1000, SENSOR_INTERVAL / 1000);
  Serial.println("══════════════════════════════════════\n");
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  server.handleClient();

  if (!apMode)
    MDNS.update();

  unsigned long now = millis();

  // ─── Fast Internal Read (every 2s) ──────────────────────────
  // Collects filtered samples into buffer
  if (now - lastInternalRead >= SENSOR_READ_INTERVAL) {
    lastInternalRead = now;
    internalSensorRead();
  }

  // ─── Publish Stable Reading (every 30s) ─────────────────────
  // Averages the buffer → updates currentDistance & currentLevel
  if (now - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = now;
    publishStableReading();
    updateFillDetection();
  }

  // ─── Log to Google Sheets & add history entry (every 5 min) ─
  if (!apMode && now - lastLogTime >= LOG_INTERVAL) {
    lastLogTime = now;
    addHistoryEntry();
    logToGoogleSheets();
  }
}
