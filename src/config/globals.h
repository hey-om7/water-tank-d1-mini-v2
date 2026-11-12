#pragma once
// ---------------------------------------------------------------------------
//  globals.h - identity + server + logging + hardware configuration for
//  Tank Monitor Device Model-1 v1 (TMM1V1), Wemos/LOLIN D1 mini v2 (ESP8266).
//
//  Before the first USB flash:
//    1. Tools -> Board: "LOLIN(WEMOS) D1 R2 & mini",
//       Flash Size: "4MB (FS:1MB OTA:~1019KB)" (any layout with OTA space).
//    2. Library Manager: install "WebSockets" by Markus Sattler.
//    3. Register this board in FirmwareManager -> Devices with its Wi-Fi MAC
//       (printed on Serial at boot) under model TMM1V1.
//    4. Bump FW_VERSION for every build you upload, and upload it with the
//       SAME version number in FirmwareManager -> Firmware.
// ---------------------------------------------------------------------------
#include <Arduino.h>

// --- Identity ----------------------------------------------------------------
#define MODEL_NUMBER      "TMM1V1"                          // = FirmwareManager model number
#define DEVICE_NAME       "Tank Monitor Device Model-1 v1"
#define FW_VERSION        "1.0.0"                           // bump on every release build
#define DEVICE_HOSTNAME   "tank-monitor-device-model-1-v1"  // DHCP host name
#define MDNS_NAME         "watertank"                       // -> http://watertank.local
#define SETUP_AP_SSID     "TMM1V1-SETUP"                    // Wi-Fi setup portal
#define SETUP_AP_PASSWORD ""                                // "" = open portal, or >= 8 chars

// --- FirmwareManager backend -------------------------------------------------
// OTA + logging try the PRIMARY server first. Only if it is unreachable (or
// the update download fails) do they use the FALLBACK (e.g. a LAN backend).
#define FM_PRIMARY_BASE_URL    "https://firmwaremanager.onrender.com"  // no trailing slash
#define FM_FALLBACK_HOST       "192.168.100.100"                       // "" = no fallback
#define FM_FALLBACK_PORT       8080                                    // 443 = https, else http
#define FM_PRIMARY_TIMEOUT_MS  20000  // hosted (Render) instances can take a while to wake
#define FM_FALLBACK_TIMEOUT_MS 8000

// --- OTA updates -------------------------------------------------------------
#define OTA_CHECK_ON_BOOT     1                             // check once Wi-Fi is up
#define OTA_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)  // then every 6 h (0 = never)
#define OTA_RETRY_INTERVAL_MS (30UL * 60UL * 1000UL)        // after a failed check

// --- Remote logging (MQTT over WebSocket, broker built into FirmwareManager) -
// The broker is derived from the backend URLs above (https -> wss://host/mqtt,
// http -> ws://host:port/mqtt). Set MQTT_BROKER_URI to pin a single broker.
#define MQTT_BROKER_URI ""
#define MQTT_USERNAME   "firmwaremanager"  // = server MQTT_USERNAME
#define MQTT_PASSWORD   "change-me"        // = server MQTT_PASSWORD

// Levels are bitmasks: 0x01 error | 0x02 warn | 0x04 info | 0x08 debug.
// FIRST-BOOT defaults only. After that the FirmwareManager console is the
// authority (Devices -> Logging); the device stores the setting in EEPROM.
#define LOG_REMOTE_LEVELS_DEFAULT 0x07  // console: error + warn + info
#define LOG_SERIAL_LEVELS_DEFAULT 0x0F  // USB serial: everything
// Compile-time ceiling (0 silent .. 4 debug): calls above it compile out.
#define LOG_LEVEL 4

// ESP8266 has ~40 KB of free heap, so these are smaller than on ESP32.
#define REMOTE_LOG_LINE_MAX         200                    // bytes per line (server max 1024)
#define REMOTE_LOG_BATCH_BYTES      768                    // max payload of one MQTT publish
#define REMOTE_LOG_BUFFER_BYTES     (4 * 1024)             // backlog while offline
#define REMOTE_LOG_FLUSH_MS         2000                   // max delay before a partial batch is sent
#define REMOTE_LOG_RETRY_MS         10000                  // WebSocket reconnect back-off
#define REMOTE_LOG_WATCHDOG_RESTART 1                      // reboot after 10 min offline with Wi-Fi up
#define REMOTE_LOG_FALLBACK_MS      60000UL                // switch broker if the current one never connects
#define REMOTE_LOG_PREFER_RETRY_MS  (10UL * 60UL * 1000UL) // retry the primary while on fallback
// TLS (wss://) needs one large contiguous block. Below this the device does
// not attempt a TLS connection, to avoid an out-of-memory crash.
#define TLS_MIN_FREE_BLOCK 18000
// Plain ws:// needs much less, but sending frames fails below this.
#define WS_MIN_FREE_HEAP 8000

// --- Wi-Fi -------------------------------------------------------------------
#define WIFI_CONNECT_TIMEOUT_MS   15000UL
#define WIFI_PORTAL_TIMEOUT_MS    180000UL  // portal after a FAILED saved network, then reboot + retry
#define WIFI_RECONNECT_INTERVAL_MS 30000UL  // link check while disconnected
#define WIFI_LOST_RESTART_MS      (3UL * 60UL * 1000UL)  // reboot after this long without Wi-Fi

// --- Power saving (battery) --------------------------------------------------
// 0 = radio always on (highest draw, ~70 mA)
// 1 = modem sleep: radio wakes for DTIM beacons (~15-20 mA average)
// 2 = light sleep: modem sleep + CPU paused during delay() (lowest draw that
//     still keeps the web page, MQTT and remote restart available)
#define POWER_SAVE_MODE      2
#define WIFI_LISTEN_INTERVAL 3     // wake every Nth DTIM beacon (1..10); higher = less power, slower replies
#define WIFI_TX_POWER_DBM    17.0f // 0..20.5; lower saves power if the signal is strong
#define LOOP_IDLE_MS         50    // delay() per loop: this is where the chip sleeps
// Gratuitous ARP keeps the router from forgetting a sleeping device (the
// dashboard would stop loading while MQTT still works).
#define ARP_ANNOUNCE_INTERVAL_MS 60000UL

// --- Pin Definitions ---------------------------------------------------------
#define TRIGGER_PIN D1  // GPIO5  -> JSN-SR04T Trig
#define ECHO_PIN    D2  // GPIO4  -> JSN-SR04T Echo
// Optional: a GPIO that switches the sensor's supply (e.g. via a P-MOSFET or
// transistor) so it draws nothing between readings. -1 = sensor always on.
#define SENSOR_POWER_PIN      -1
#define SENSOR_POWER_ON_LEVEL HIGH
#define SENSOR_WARMUP_MS      300

// --- EEPROM ------------------------------------------------------------------
#define EEPROM_SIZE  512
#define EEPROM_MAGIC 0xA5

// Byte 0:       Magic byte (0xA5 = configured)
// Byte 1-32:    WiFi SSID (32 bytes)
// Byte 33-96:   WiFi Password (64 bytes)
// Byte 97-98:   Tank depth: sensor to tank bottom (cm) [uint16]   (was "tank empty distance")
// Byte 99-100:  Upper gap: sensor to the 100% water line (cm) [uint16] (was "tank full distance")
// Byte 101-356: unused (was the Google Sheets webhook URL)
// Byte 357-388: Device name (32 bytes)
// Byte 389-420: Firmware version string (32 bytes)
// Byte 421-422: Tank girth/circumference (cm) [uint16]
// Byte 423:     Number of tanks [uint8]
// Byte 424:     Log-level magic (0x5A = levels below are valid)
// Byte 425:     Remote (console) log level mask
// Byte 426:     Serial log level mask
// Byte 427-428: Lower gap: tank bottom to the 0% water line (cm) [uint16]  (new)
// Byte 429:     Lower-gap magic (0x3C = byte 427-428 valid)               (new)
#define EEPROM_SSID_OFFSET             1
#define EEPROM_PASS_OFFSET             33
#define EEPROM_TANK_DEPTH_OFFSET       97
#define EEPROM_UPPER_GAP_OFFSET        99
#define EEPROM_NAME_OFFSET             357
#define EEPROM_FIRMWARE_VERSION_OFFSET 389
#define EEPROM_FIRMWARE_VERSION_SIZE   32
#define EEPROM_GIRTH_OFFSET            421
#define EEPROM_TANK_COUNT_OFFSET       423
#define EEPROM_LOG_MAGIC_OFFSET        424
#define EEPROM_LOG_MAGIC               0x5A
#define EEPROM_LOG_REMOTE_OFFSET       425
#define EEPROM_LOG_SERIAL_OFFSET       426
#define EEPROM_LOWER_GAP_OFFSET        427
#define EEPROM_LOWER_GAP_MAGIC_OFFSET  429
#define EEPROM_LOWER_GAP_MAGIC         0x3C

// --- Tank defaults (first boot) ----------------------------------------------
#define DEFAULT_TANK_DEPTH_CM 200
#define DEFAULT_UPPER_GAP_CM  20
#define DEFAULT_LOWER_GAP_CM  0

// --- Sensor ------------------------------------------------------------------
#define SENSOR_INTERVAL 30000  // read every 30 seconds
#define SPIKE_THRESHOLD 5.0    // max allowed jump (cm) from the last valid reading

// --- Level smoothing ---------------------------------------------------------
// Applied to every accepted reading, in this order:
//  1. velocity clamp: a single step can move the level at most this much
//  2. deadzone: smaller changes keep the level flat (idle-tank jitter)
//  3. EMA: smoothed = ALPHA * new + (1 - ALPHA) * previous
#define LEVEL_VELOCITY_CLAMP 20.0  // % per reading (glitch safety net)
#define LEVEL_DEADZONE       0.5   // %
#define LEVEL_EMA_ALPHA      0.7

// --- Fill detection ----------------------------------------------------------
#define FILL_SAMPLE_INTERVAL 300000  // one level sample every 5 min
#define FILL_WINDOW          10      // lookback (10 samples = 50 min)
#define FILL_THRESHOLD       3.0     // >3% rise in the window = filling

// --- Health ------------------------------------------------------------------
#define HEALTH_LOG_INTERVAL 60000  // heap / RSSI line every 60 s (INFO)
