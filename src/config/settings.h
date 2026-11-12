#pragma once
// ---------------------------------------------------------------------------
//  settings.h - device settings stored in EEPROM (layout in globals.h).
//
//  Tank geometry (sensor mounted at the top, looking down):
//
//      sensor ──┬──────────────
//               │ upperGap        distance <= upperGap          -> 100 %
//      100 % ───┼──────────────
//               │ measured range  0..100 % maps linearly in here
//        0 % ───┼──────────────
//               │ lowerGap        distance >= depth - lowerGap  -> 0 %
//      bottom ──┴──────────────
//               depth = sensor to bottom
//
//  Settings::get()     current settings (loaded by Settings::load()).
//  Settings::save()    write everything back + commit.
//  Settings::valid()   true when the gaps leave a usable range.
//  HEADER-ONLY. EEPROM.begin() must be called before load().
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <EEPROM.h>
#include "globals.h"
#include "../logging/custom_mqtt_logging.h"

namespace Settings {

struct Config {
  char ssid[33];
  char pass[65];
  char name[33];
  uint16_t tankDepth;  // cm, sensor to tank bottom
  uint16_t upperGap;   // cm, sensor to the 100 % line
  uint16_t lowerGap;   // cm, tank bottom to the 0 % line
  uint16_t tankGirth;  // cm, kept for compatibility with older firmware
  uint8_t tankCount;   // kept for compatibility with older firmware
};

inline Config& get() {
  static Config c;
  return c;
}

namespace detail {

inline uint16_t read16(int at) { return EEPROM.read(at) | (EEPROM.read(at + 1) << 8); }
inline void write16(int at, uint16_t v) {
  EEPROM.write(at, v & 0xFF);
  EEPROM.write(at + 1, (v >> 8) & 0xFF);
}
inline void readStr(int at, char* out, size_t len) {  // out has len + 1 bytes
  for (size_t i = 0; i < len; i++) out[i] = EEPROM.read(at + i);
  out[len] = '\0';
}
inline void writeStr(int at, const char* s, size_t len) {
  for (size_t i = 0; i < len; i++) EEPROM.write(at + i, s[i]);
}

}  // namespace detail

/** Distance (cm) that reads as 100 %. */
inline float fullDistance() { return get().upperGap; }
/** Distance (cm) that reads as 0 %. */
inline float emptyDistance() { return static_cast<float>(get().tankDepth) - get().lowerGap; }
/** Usable range: the empty line must be below the full line. */
inline bool valid() { return emptyDistance() > fullDistance() + 1.0f; }

inline bool hasWifi() { return get().ssid[0] != '\0'; }

inline void applyDefaults() {
  Config& c = get();
  if (c.tankDepth == 0 || c.tankDepth == 0xFFFF) c.tankDepth = DEFAULT_TANK_DEPTH_CM;
  if (c.upperGap == 0xFFFF) c.upperGap = DEFAULT_UPPER_GAP_CM;
  if (c.lowerGap == 0xFFFF) c.lowerGap = DEFAULT_LOWER_GAP_CM;
  if (c.tankCount == 0 || c.tankCount == 0xFF) c.tankCount = 1;
  if (c.tankGirth == 0xFFFF) c.tankGirth = 0;
  if (!c.name[0] || static_cast<uint8_t>(c.name[0]) == 0xFF) strncpy(c.name, "WaterTank", sizeof c.name - 1);
}

inline void load() {
  Config& c = get();
  memset(&c, 0, sizeof c);
  if (EEPROM.read(0) != EEPROM_MAGIC) {
    LOGW("CONFIG", "no saved config in EEPROM - using defaults (setup portal will open)");
    c.upperGap = DEFAULT_UPPER_GAP_CM;
    c.lowerGap = DEFAULT_LOWER_GAP_CM;
    applyDefaults();
    return;
  }
  detail::readStr(EEPROM_SSID_OFFSET, c.ssid, 32);
  detail::readStr(EEPROM_PASS_OFFSET, c.pass, 64);
  detail::readStr(EEPROM_NAME_OFFSET, c.name, 32);
  c.tankDepth = detail::read16(EEPROM_TANK_DEPTH_OFFSET);
  c.upperGap = detail::read16(EEPROM_UPPER_GAP_OFFSET);
  // Older firmware never wrote the lower gap: treat it as 0 until saved.
  c.lowerGap = EEPROM.read(EEPROM_LOWER_GAP_MAGIC_OFFSET) == EEPROM_LOWER_GAP_MAGIC
                   ? detail::read16(EEPROM_LOWER_GAP_OFFSET)
                   : DEFAULT_LOWER_GAP_CM;
  c.tankGirth = detail::read16(EEPROM_GIRTH_OFFSET);
  c.tankCount = EEPROM.read(EEPROM_TANK_COUNT_OFFSET);
  applyDefaults();
  LOGI("CONFIG", "loaded: name=%s ssid=%s depth=%ucm upperGap=%ucm lowerGap=%ucm (100%%@%.0fcm 0%%@%.0fcm)",
       c.name, c.ssid, c.tankDepth, c.upperGap, c.lowerGap, fullDistance(), emptyDistance());
  if (!valid()) LOGW("CONFIG", "gaps leave no measuring range: depth - lowerGap must be > upperGap");
}

inline void save() {
  const Config& c = get();
  EEPROM.write(0, EEPROM_MAGIC);
  detail::writeStr(EEPROM_SSID_OFFSET, c.ssid, 32);
  detail::writeStr(EEPROM_PASS_OFFSET, c.pass, 64);
  detail::writeStr(EEPROM_NAME_OFFSET, c.name, 32);
  detail::write16(EEPROM_TANK_DEPTH_OFFSET, c.tankDepth);
  detail::write16(EEPROM_UPPER_GAP_OFFSET, c.upperGap);
  detail::write16(EEPROM_LOWER_GAP_OFFSET, c.lowerGap);
  EEPROM.write(EEPROM_LOWER_GAP_MAGIC_OFFSET, EEPROM_LOWER_GAP_MAGIC);
  detail::write16(EEPROM_GIRTH_OFFSET, c.tankGirth);
  EEPROM.write(EEPROM_TANK_COUNT_OFFSET, c.tankCount);
  EEPROM.commit();
  LOGI("CONFIG", "saved: name=%s depth=%ucm upperGap=%ucm lowerGap=%ucm", c.name, c.tankDepth, c.upperGap,
       c.lowerGap);
}

}  // namespace Settings
