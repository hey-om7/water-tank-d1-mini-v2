/*
 * globals.h — Shared constants, types, and extern declarations
 * =============================================================
 * D1 Mini (ESP8266) water tank monitor, integrated with FirmwareManager:
 *  - OTA updates from FirmwareManager (primary Render server, LAN fallback)
 *  - Remote logging over MQTT (WebSocket) to the FirmwareManager console,
 *    levels controlled from FirmwareManager -> Devices -> Logging
 *  - Remote restart from the console
 *  - Reset-reason / crash diagnostics reported on every boot
 *  - On-device level smoothing (velocity clamp -> deadzone -> EMA)
 *
 * Before the first USB flash:
 *   Register this board in FirmwareManager -> Devices with its Wi-Fi MAC
 *   (printed on Serial at boot) under model MODEL_NUMBER.
 * For every release: bump FW_VERSION and upload the .bin with the SAME
 * version number in FirmwareManager -> Firmware.
 */

#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <ESP8266WebServer.h>

// ─── Identity ────────────────────────────────────────────────
#define MODEL_NUMBER "TMM1V1" // = FirmwareManager model number
#define DEVICE_NAME "Tank Monitor Device Model-1 v1"
#define FW_VERSION "2.1.1" // bump on every release build

// ─── FirmwareManager backend ─────────────────────────────────
// OTA + logging try the PRIMARY server first. Only if it is unreachable (or
// the update download fails) do they use the FALLBACK (LAN backend).
#define FM_PRIMARY_BASE_URL "https://firmwaremanager.onrender.com" // no trailing slash
#define FM_FALLBACK_HOST "192.168.100.100" // "" = no fallback
#define FM_FALLBACK_PORT 8080              // 443 = https, anything else = http
#define FM_PRIMARY_TIMEOUT_MS 20000 // Render instances can take a while to wake
#define FM_FALLBACK_TIMEOUT_MS 8000

// ─── OTA updates ─────────────────────────────────────────────
#define OTA_CHECK_ON_BOOT 1                                // check once Wi-Fi is up
#define OTA_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL) // then every 6 h (0 = never)
#define OTA_RETRY_INTERVAL_MS (30UL * 60UL * 1000UL)       // after a failed check

// ─── Remote logging (MQTT over WebSocket, broker built into FirmwareManager)
// The broker is derived from the backend URLs above (https -> wss://host/mqtt,
// http -> ws://host:port/mqtt). Set MQTT_BROKER_URI to pin a single broker.
#define MQTT_BROKER_URI ""
#define MQTT_USERNAME "firmwaremanager" // = server MQTT_USERNAME
#define MQTT_PASSWORD "change-me"       // = server MQTT_PASSWORD

// Levels are bitmasks: 0x01 error | 0x02 warn | 0x04 info | 0x08 debug.
// FIRST-BOOT defaults only. After that the FirmwareManager console is the
// authority (Devices -> Logging); the device stores the setting in EEPROM.
#define LOG_REMOTE_LEVELS_DEFAULT 0x07 // console: error + warn + info
#define LOG_SERIAL_LEVELS_DEFAULT 0x0F // USB serial: everything
// Compile-time ceiling (0 silent .. 4 debug): calls above it compile out.
#define LOG_LEVEL 4

// ESP8266 has ~40 KB of free heap, so these are smaller than on ESP32.
#define REMOTE_LOG_LINE_MAX 200          // bytes per line (server max 1024)
#define REMOTE_LOG_BATCH_BYTES 768       // max payload of one MQTT publish
#define REMOTE_LOG_BUFFER_BYTES (4 * 1024) // backlog while offline
#define REMOTE_LOG_FLUSH_MS 2000         // max delay before a partial batch is sent
#define REMOTE_LOG_RETRY_MS 10000        // WebSocket reconnect back-off
#define REMOTE_LOG_WATCHDOG_RESTART 1    // reboot after 10 min offline with Wi-Fi up
#define REMOTE_LOG_FALLBACK_MS 60000UL   // switch broker if the current one never connects
#define REMOTE_LOG_PREFER_RETRY_MS (10UL * 60UL * 1000UL) // retry primary while on fallback
// TLS (wss://) needs one large contiguous block. Below this the device does
// not attempt a TLS connection, to avoid an out-of-memory crash.
#define TLS_MIN_FREE_BLOCK 18000
// Plain ws:// needs much less, but sending frames fails below this.
#define WS_MIN_FREE_HEAP 8000

// ─── Pin Definitions ─────────────────────────────────────────
#define TRIGGER_PIN D1 // GPIO5
#define ECHO_PIN D2    // GPIO4

// ─── EEPROM Constants ────────────────────────────────────────
#define EEPROM_SIZE 512
#define EEPROM_MAGIC 0xA5

// ─── EEPROM Layout ───────────────────────────────────────────
// Byte 0:       Magic byte (0xA5 = configured)
// Byte 1-32:    WiFi SSID (32 bytes)
// Byte 33-96:   WiFi Password (64 bytes)
// Byte 97-98:   Tank empty distance (cm) [uint16]
// Byte 99-100:  Tank full distance (cm) [uint16]
// Byte 101-356: unused (was the Google Sheets webhook URL)
// Byte 357-388: Device name (32 bytes)
// Byte 389-420: Firmware version string (32 bytes)
// Byte 421-422: Tank girth/circumference (cm) [uint16]
// Byte 423:     Number of tanks [uint8]
// Byte 424:     Log-level magic (0x5A = levels below are valid)
// Byte 425:     Remote (console) log level mask
// Byte 426:     Serial log level mask
#define EEPROM_FIRMWARE_VERSION_OFFSET 389
#define EEPROM_FIRMWARE_VERSION_SIZE 32
#define EEPROM_LOG_MAGIC_OFFSET 424
#define EEPROM_LOG_MAGIC 0x5A
#define EEPROM_LOG_REMOTE_OFFSET 425
#define EEPROM_LOG_SERIAL_OFFSET 426

// ─── WiFi Constants ──────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT 15000 // 15 seconds
#define AP_SSID "WaterTank-Setup"
#define AP_PASS "" // Open network for easy setup
#define MDNS_NAME "watertank"

// ─── Sensor Constants ────────────────────────────────────────
#define SENSOR_INTERVAL 30000 // Read every 30 seconds
#define SPIKE_THRESHOLD 5.0   // Max allowed jump (cm) from last valid

// ─── Level smoothing (moved here from the old Sheets script) ─
// Applied to every accepted reading, in this order:
//  1. velocity clamp: a single step can move the level at most this much
//  2. deadzone: smaller changes keep the level flat (idle-tank jitter)
//  3. EMA: smoothed = ALPHA * new + (1 - ALPHA) * previous
#define LEVEL_VELOCITY_CLAMP 20.0 // % per reading (glitch safety net)
#define LEVEL_DEADZONE 0.5        // %
#define LEVEL_EMA_ALPHA 0.7

// ─── Fill detection ──────────────────────────────────────────
#define FILL_SAMPLE_INTERVAL 300000 // one level sample every 5 min
#define FILL_WINDOW 10              // lookback (10 samples = 50 min)
#define FILL_THRESHOLD 3.0          // >3% rise in the window = filling

// ─── Health ──────────────────────────────────────────────────
#define HEALTH_LOG_INTERVAL 60000 // heap / RSSI line every 60 s (INFO)

// ─── Config Struct ───────────────────────────────────────────
struct Config {
  char ssid[33];
  char pass[65];
  uint16_t tankEmpty; // Distance when tank is empty (sensor to bottom)
  uint16_t tankFull;  // Distance when tank is full (sensor to water surface)
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

// Fill detection state (updated every FILL_SAMPLE_INTERVAL)
extern bool isFilling;
extern float fillRate;

#endif // GLOBALS_H
