/*
 * config_logic.h — EEPROM Configuration Management
 * ==================================================
 * Read/write device config (WiFi, tank params, webhook, name)
 * from EEPROM. Includes factory reset functionality.
 */

#ifndef CONFIG_LOGIC_H
#define CONFIG_LOGIC_H

#include "globals.h"
#include <EEPROM.h>

// ─── Load Config from EEPROM ─────────────────────────────────
inline void loadConfig() {
  memset(&config, 0, sizeof(config));

  if (EEPROM.read(0) != EEPROM_MAGIC) {
    Serial.println("[CONFIG] No saved config found");
    config.tankEmpty = 200; // Default: 200cm when empty
    config.tankFull = 20;   // Default: 20cm when full
    strncpy(config.name, "WaterTank", sizeof(config.name) - 1);
    return;
  }

  // Read SSID
  for (int i = 0; i < 32; i++)
    config.ssid[i] = EEPROM.read(1 + i);
  config.ssid[32] = '\0';

  // Read password
  for (int i = 0; i < 64; i++)
    config.pass[i] = EEPROM.read(33 + i);
  config.pass[64] = '\0';

  // Read tank parameters
  config.tankEmpty = EEPROM.read(97) | (EEPROM.read(98) << 8);
  config.tankFull = EEPROM.read(99) | (EEPROM.read(100) << 8);

  // Read webhook URL
  for (int i = 0; i < 256; i++)
    config.webhook[i] = EEPROM.read(101 + i);
  config.webhook[256] = '\0';

  // Read device name
  for (int i = 0; i < 32; i++)
    config.name[i] = EEPROM.read(357 + i);
  config.name[32] = '\0';

  // Validate
  if (config.tankEmpty == 0)
    config.tankEmpty = 200;
  if (config.tankFull == 0)
    config.tankFull = 20;
  if (strlen(config.name) == 0)
    strncpy(config.name, "WaterTank", sizeof(config.name) - 1);

  Serial.printf("[CONFIG] Loaded: SSID=%s, Empty=%dcm, Full=%dcm\n",
                config.ssid, config.tankEmpty, config.tankFull);
}

// ─── Save Config to EEPROM ───────────────────────────────────
inline void saveConfigToEEPROM() {
  EEPROM.write(0, EEPROM_MAGIC);

  for (int i = 0; i < 32; i++)
    EEPROM.write(1 + i, config.ssid[i]);
  for (int i = 0; i < 64; i++)
    EEPROM.write(33 + i, config.pass[i]);

  EEPROM.write(97, config.tankEmpty & 0xFF);
  EEPROM.write(98, (config.tankEmpty >> 8) & 0xFF);
  EEPROM.write(99, config.tankFull & 0xFF);
  EEPROM.write(100, (config.tankFull >> 8) & 0xFF);

  for (int i = 0; i < 256; i++)
    EEPROM.write(101 + i, config.webhook[i]);
  for (int i = 0; i < 32; i++)
    EEPROM.write(357 + i, config.name[i]);

  EEPROM.commit();
  Serial.println("[CONFIG] Saved to EEPROM");
}

// ─── Factory Reset ───────────────────────────────────────────
inline void factoryReset() {
  Serial.println("[CONFIG] === FACTORY RESET ===");
  for (int i = 0; i < EEPROM_SIZE; i++)
    EEPROM.write(i, 0);
  EEPROM.commit();
  delay(500);
  ESP.restart();
}

#endif // CONFIG_LOGIC_H
