/*
 * crumbs.h — Breadcrumbs that survive a reset
 * ==================================================
 * Keeps a small record in RTC user memory: what the firmware was doing
 * ("stage"), uptime and heap figures, and the reason for any deliberate
 * restart. RTC memory survives software restarts, watchdog resets,
 * exceptions and the RST button, but NOT a power loss. So on the next boot:
 *   - valid record + crash/WDT reset  -> we know the stage it died in;
 *   - valid record + software restart -> we know which code restarted it;
 *   - power-on reset (record lost)    -> the board lost power / browned out.
 * The boot report itself is logged by Diag::reportBoot() in
 * custom_mqtt_logging.h, so it reaches the FirmwareManager console.
 *
 * No logging in here (this header is included by the logger).
 */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <Arduino.h>
#include <user_interface.h>

namespace Crumbs {

// User RTC memory offsets 0..31 (128 bytes) hold the eboot command used by
// OTA, so the record starts at block 64 (byte 256).
static constexpr uint32_t kRtcBlock = 64;
static constexpr uint32_t kMagic = 0x57544D31; // "WTM1"

struct Record {
  uint32_t magic;
  uint32_t bootCount;
  uint32_t uptimeSec;   // uptime at the last update
  uint32_t freeHeap;    // at the last update
  uint32_t minFreeHeap; // lowest seen this session
  uint16_t maxBlock;    // largest free block at the last update
  uint8_t frag;         // heap fragmentation % at the last update
  uint8_t pad;
  char stage[24];       // what the firmware was doing
  char note[80];        // reason for a deliberate restart ("" = none)
  uint32_t crc;
};
static_assert(sizeof(Record) % 4 == 0, "RTC record must be 4-byte aligned");
static_assert(sizeof(Record) <= 256, "RTC record too big");

inline Record &R() {
  static Record r;
  return r;
}

// Snapshot of the PREVIOUS session, taken once at boot.
inline Record &Prev() {
  static Record p;
  return p;
}
inline bool &prevValid() {
  static bool v = false;
  return v;
}

inline uint32_t crcOf(const Record &r) {
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&r);
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < offsetof(Record, crc); ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
  }
  return ~c;
}

inline void save() {
  R().crc = crcOf(R());
  ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t *>(&R()), sizeof(Record));
}

// Refresh uptime + heap figures (cheap; call from loop every few seconds).
inline void sample() {
  uint32_t fh = ESP.getFreeHeap();
  R().uptimeSec = millis() / 1000;
  R().freeHeap = fh;
  if (fh < R().minFreeHeap) R().minFreeHeap = fh;
  R().maxBlock = ESP.getMaxFreeBlockSize();
  R().frag = ESP.getHeapFragmentation();
  save();
}

// Mark what the firmware is about to do. Cheap (no heap walk), so it can be
// called every loop. Keep names short.
inline void stage(const char *s) {
  if (strncmp(R().stage, s, sizeof(R().stage)) == 0) return;
  strncpy(R().stage, s, sizeof(R().stage) - 1);
  R().stage[sizeof(R().stage) - 1] = '\0';
  R().uptimeSec = millis() / 1000;
  save();
}

// Store the reason right before a deliberate ESP.restart().
inline void note(const char *n) {
  strncpy(R().note, n, sizeof(R().note) - 1);
  R().note[sizeof(R().note) - 1] = '\0';
  sample();
}

// Call once, first thing in setup(): keeps the previous record in Prev()
// and starts a fresh one for this session.
inline void begin() {
  Record old;
  ESP.rtcUserMemoryRead(kRtcBlock, reinterpret_cast<uint32_t *>(&old), sizeof(Record));
  prevValid() = old.magic == kMagic && old.crc == crcOf(old);
  if (prevValid()) {
    old.stage[sizeof(old.stage) - 1] = '\0';
    old.note[sizeof(old.note) - 1] = '\0';
    Prev() = old;
  } else {
    memset(&Prev(), 0, sizeof(Record));
  }
  memset(&R(), 0, sizeof(Record));
  R().magic = kMagic;
  R().bootCount = prevValid() ? old.bootCount + 1 : 1;
  R().minFreeHeap = ESP.getFreeHeap();
  stage("boot");
}

} // namespace Crumbs

#endif // DIAGNOSTICS_H
