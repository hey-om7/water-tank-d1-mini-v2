/*
 * globals.h — Shared constants, types, and extern declarations
 * =============================================================
 * Central definitions used by all modules in the Water Tank firmware.
 */

#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <ESP8266WebServer.h>

// ─── Pin Definitions ──────────────────────────────────────────
#define TRIGGER_PIN D1 // GPIO5
#define ECHO_PIN D2    // GPIO4

// ─── EEPROM Constants ─────────────────────────────────────────
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xA5

// ─── EEPROM Layout ───────────────────────────────────────────
// Byte 0:       Magic byte (0xA5 = configured)
// Byte 1-32:    WiFi SSID (32 bytes)
// Byte 33-96:   WiFi Password (64 bytes)
// Byte 97-98:   Tank empty distance (cm) [uint16]
// Byte 99-100:  Tank full distance (cm) [uint16]
// Byte 101-356: Webhook URL (256 bytes)
// Byte 357-388: Device name (32 bytes)
// Byte 389-420: Firmware version string (32 bytes)
// Byte 421-422: Tank girth/circumference (cm) [uint16]
// Byte 423:     Number of tanks [uint8]
#define EEPROM_FIRMWARE_VERSION_OFFSET 389
#define EEPROM_FIRMWARE_VERSION_SIZE 32

// ─── WiFi Constants ──────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT 15000 // 15 seconds
#define AP_SSID "WaterTank-Setup"
#define AP_PASS "" // Open network for easy setup
#define MDNS_NAME "watertank"

// ─── Sensor Constants ────────────────────────────────────────
#define SENSOR_SAMPLES 7          // Odd number for median filter
#define SENSOR_READ_INTERVAL 2000 // Internal fast read every 2s
#define SENSOR_INTERVAL 30000     // Publish filtered value every 30s

// ─── Sensor Filtering Constants ──────────────────────────────
#define SPIKE_THRESHOLD 5.0   // Max allowed jump (cm) from last valid
#define EMA_ALPHA 0.3         // Exponential moving average factor (0-1)
#define FILTER_BUFFER_SIZE 15 // Internal buffer for 30s of 2s reads

// ─── Fill Detection Constants ────────────────────────────────
#define FILL_WINDOW 10     // Lookback window (readings)
#define FILL_THRESHOLD 3.0 // >3% rise in window = filling

// ─── Logging & History ───────────────────────────────────────
#define LOG_INTERVAL 300000 // Log to Google Sheets every 5 min
#define HISTORY_SIZE 288    // 24h at 5-min intervals

// ─── OTA API Endpoints ──────────────────────────────────────
#define OTA_VERSION_CHECK_URL                                                  \
  "http://oms-macbook-air.local:8080/api/v1/device/firmware/version"
#define OTA_FIRMWARE_DOWNLOAD_URL                                              \
  "http://oms-macbook-air.local:8080/api/v1/device/firmware"

// ─── Default Firmware Version ────────────────────────────────
#define DEFAULT_FIRMWARE_VERSION "1.0.0"

// ─── Config Struct ───────────────────────────────────────────
struct Config {
  char ssid[33];
  char pass[65];
  uint16_t tankEmpty; // Distance when tank is empty (sensor to bottom)
  uint16_t tankFull;  // Distance when tank is full (sensor to water surface)
  char webhook[257];
  char name[33];
  uint16_t tankGirth; // Tank circumference in cm
  uint8_t tankCount;  // Number of connected tanks
};

// ─── History Entry ───────────────────────────────────────────
struct HistoryEntry {
  uint32_t timestamp;
  float level;
};

// ─── Global State (extern declarations) ──────────────────────
// Defined in water-tank-info.ino
extern ESP8266WebServer server;
extern Config config;
extern bool apMode;

// Sensor state
extern float currentDistance;
extern float currentLevel;
extern float currentLiters;
extern unsigned long lastSensorRead;

// Sensor filter state
extern float filterBuffer[];
extern int filterBufIdx;
extern int filterBufCount;
extern float lastValidDistance;
extern float emaDistance;
extern unsigned long lastInternalRead;

// Fill detection state
extern float levelHistory[FILL_WINDOW];
extern int levelHistIdx;
extern bool isFilling;
extern float fillRate;
extern float fillEta;
extern unsigned long lastFillCheck;

// History buffer
extern HistoryEntry historyBuf[HISTORY_SIZE];
extern int historyHead;
extern int historyCount;

// Logging
extern unsigned long lastLogTime;

#endif // GLOBALS_H
