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
#include "customHttpLogging.h"
#include "ota_update_logic.h"
#include "sensor_logic.h"
#include "wifi_logic.h"
// ─── Global Variable Definitions ─────────────────────────────
// (Declared as extern in globals.h)
ESP8266WebServer server(80);
Config config;
bool apMode = false;

// Sensor state
// currentDistance / currentRawDistance are initialised to -1 (a value
// the sensor cannot physically produce) so the calculators can detect
// "no valid reading yet" and return 0 instead of computing against a
// bogus distance==0 (which would give a spurious 100 % full).
float currentDistance = -1;
float currentLevel = 0;  // 0-100%
float currentLiters = 0; // Total liters remaining
unsigned long lastSensorRead = 0;

// Sensor filter state
float filterBuffer[FILTER_BUFFER_SIZE];
int filterBufIdx = 0;
int filterBufCount = 0;
float lastValidDistance = -1;
float currentRawDistance = -1;
int spikeCount = 0;
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
unsigned long lastWifiCheck = 0;

// ══════════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(100);

  // Enable the hardware watchdog explicitly (~8 s). This is insurance
  // on top of the software WDT — if any part of the stack ever hangs
  // in a state where the SW WDT still gets fed but forward progress
  // has stopped, the HW WDT will force a reboot instead of leaving the
  // device silently unresponsive.
  ESP.wdtDisable();
  ESP.wdtEnable(WDTO_8S);

  printLog("\n\n══════════════════════════════════════");
  printLog("  Water Tank Monitor — Starting Up");
  printLog("══════════════════════════════════════");

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
    printLog("\n[BOOT] Checking for firmware updates...");
    checkAndPerformOTA();

    // 5. Setup mDNS
    setupMDNS();

    // 6. Setup dashboard routes
    setupRoutes();
  } else {
    // No saved WiFi or connection failed → AP mode
    printLog("[BOOT] No WiFi credentials or connection failed");
    printLog("[BOOT] Starting Access Point for setup...");
    startAP();
    setupCaptivePortalRoutes();
  }

  // 7. Start web server
  server.begin();

  // 8. Take initial sensor reading
  printLog("[BOOT] Taking initial sensor readings...");
  for (int i = 0; i < 5; i++) {
    internalSensorRead();
    delay(500);
  }
  publishStableReading();

  printLog("\n══════════════════════════════════════");
  printLog("  Boot complete — system running");
  printLog("  Mode: %s", apMode ? "Access Point" : "WiFi Station");
  printLog("  Sensor: read every %ds, publish every %ds",
               SENSOR_READ_INTERVAL / 1000, SENSOR_INTERVAL / 1000);
  printLog("══════════════════════════════════════");
}

// ══════════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════════
void loop() {
  // Feed the watchdog at the top of every iteration.
  ESP.wdtFeed();

  server.handleClient();

  if (!apMode)
    maintainMDNS();

  unsigned long now = millis();

  // ─── Heap-health monitor ─────────────────────────────────────
  // The main long-term failure mode on this platform is heap
  // fragmentation: after hours/days, the largest free block
  // shrinks below what BearSSL (~16 KB contiguous) or the web
  // server needs, and network I/O silently fails while loop()
  // keeps running. We check every minute and force a reboot
  // BEFORE that happens, using the largest-free-block metric
  // (not just total free heap, which can be misleading).
  {
    static unsigned long lastHeapCheck = 0;
    static uint8_t   heapCriticalStrikes = 0;
    const  unsigned long HEAP_CHECK_INTERVAL = 60000; // 1 minute

    if (now - lastHeapCheck >= HEAP_CHECK_INTERVAL) {
      lastHeapCheck = now;
      uint32_t freeHeap  = ESP.getFreeHeap();
      uint16_t maxBlock  = ESP.getMaxFreeBlockSize();
      uint8_t  fragPct   = ESP.getHeapFragmentation();

      // Thresholds tuned for BearSSL headroom on ESP8266:
      //  - <18 KB max contiguous block cannot host SSL context reliably.
      //  - >70% fragmentation is the danger zone.
      //  - <10 KB total free heap is already failing.
      bool critical = (freeHeap < 10000) ||
                      (fragPct  > 70)    ||
                      (maxBlock < 18000);

      if (critical) {
        heapCriticalStrikes++;
        printLog("[HEAP] critical: free=%u maxBlock=%u frag=%u%% strike=%u",
                 freeHeap, maxBlock, fragPct, heapCriticalStrikes);
        // 3 consecutive critical readings (~3 min) → reboot to recover.
        if (heapCriticalStrikes >= 3) {
          printLog("[HEAP] Rebooting to recover heap.");
          delay(200);
          ESP.restart();
        }
      } else {
        heapCriticalStrikes = 0;
      }
    }
  }

  // ─── WiFi keep-alive ────────────────────────────────────────
  // Rely on the SDK's built-in auto-reconnect (enabled in
  // connectWiFi). We do NOT call WiFi.disconnect() here — that
  // tears down the auto-reconnect state machine and can leave the
  // radio permanently down. We only nudge with reconnect() and, if
  // that fails for ~3 min, we reboot to escape any wedged state.
  if (!apMode && now - lastWifiCheck >= 30000) {
    lastWifiCheck = now;
    static uint8_t wifiFailCount = 0;

    if (WiFi.status() != WL_CONNECTED) {
      wifiFailCount++;
      printLog("[WIFI] Not connected (fail #%u), nudging reconnect...",
               wifiFailCount);
      WiFi.reconnect();

      if (wifiFailCount >= 6) {   // 6 × 30 s = 3 minutes
        printLog("[WIFI] Persistent connection loss — rebooting.");
        delay(200);
        ESP.restart();
      }
    } else {
      wifiFailCount = 0;
    }
  }



  // ─── 3-sample burst read (every 60s) ───────────────────────
  // Collects one averaged+spike-rejected sample into the 5-slot buffer
  if (now - lastInternalRead >= SENSOR_READ_INTERVAL) {
    lastInternalRead = now;
    internalSensorRead();
    // Yield after the blocking burst so the web server can handle
    // any queued requests before the next loop iteration.
    server.handleClient();
  }

  // ─── Publish median + update fill detection (every 5 min) ───
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
    // Yield after the blocking HTTPS call so the web server can
    // catch up on any requests that arrived during the POST.
    server.handleClient();
  }
}
