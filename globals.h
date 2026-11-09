/*
 * globals.h — Shared constants, types, and extern declarations
 * =============================================================
 * Minimal version: sensor + web dashboard + OTA + restart
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
// Byte 101-356: Webhook URL (256 bytes) [kept for EEPROM compat]
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
// Simple pipeline: read every 30s, publish immediately
#define SENSOR_INTERVAL 30000 // Read + publish every 30 seconds

// ─── Sensor Filtering Constants ──────────────────────────────
#define SPIKE_THRESHOLD 5.0 // Max allowed jump (cm) from last valid

// ─── OTA API Endpoints ──────────────────────────────────────
#define OTA_VERSION_CHECK_URL                                                  \
  "http://oms-macbook-air.local:8080/api/v1/device/firmware/version"
#define OTA_FIRMWARE_DOWNLOAD_URL                                              \
  "http://oms-macbook-air.local:8080/api/v1/device/firmware"

#define baseLoggingUrl "http://oms-macbook-air.local:8080/api/v1/device/logs"

// ─── Default Firmware Version ────────────────────────────────
#define DEFAULT_FIRMWARE_VERSION "2.0.0"

// ─── Config Struct ───────────────────────────────────────────
struct Config {
  char ssid[33];
  char pass[65];
  uint16_t tankEmpty; // Distance when tank is empty (sensor to bottom)
  uint16_t tankFull;  // Distance when tank is full (sensor to water surface)
  char webhook[257];  // Kept for EEPROM layout compatibility
  char name[33];
  uint16_t tankGirth; // Tank circumference in cm
  uint8_t tankCount;  // Number of connected tanks
};

// ─── Global State (extern declarations) ──────────────────────
extern ESP8266WebServer server;
extern Config config;
extern bool apMode;

// Sensor state
extern float currentDistance;
extern float currentLevel;
extern unsigned long lastSensorRead;

// Sensor filter state
extern float lastValidDistance;
extern int spikeCount;

#endif // GLOBALS_H
