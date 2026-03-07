/*
 * ota_update_logic.h — OTA Firmware Update System
 * =================================================
 * Automatic OTA update mechanism that runs during setup().
 *
 * Flow:
 *  1. Read stored firmware version from EEPROM
 *  2. HTTP GET version check endpoint with device MAC
 *  3. Compare versions using semantic versioning
 *  4. If newer version available → download & install via HTTPUpdate
 *  5. Save new version to EEPROM, restart device
 */

#ifndef OTA_UPDATE_LOGIC_H
#define OTA_UPDATE_LOGIC_H

#include "globals.h"
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>

// ─── Save Firmware Version to EEPROM ─────────────────────────
inline void saveFirmwareVersion(const String &version) {
  for (int i = 0; i < EEPROM_FIRMWARE_VERSION_SIZE; i++) {
    if (i < (int)version.length()) {
      EEPROM.write(EEPROM_FIRMWARE_VERSION_OFFSET + i, version.charAt(i));
    } else {
      EEPROM.write(EEPROM_FIRMWARE_VERSION_OFFSET + i, 0);
    }
  }
  EEPROM.commit();
  Serial.printf("[OTA] Firmware version saved: %s\n", version.c_str());
}

// ─── Load Firmware Version from EEPROM ───────────────────────
inline String loadFirmwareVersion() {
  char version[EEPROM_FIRMWARE_VERSION_SIZE + 1];
  memset(version, 0, sizeof(version));

  for (int i = 0; i < EEPROM_FIRMWARE_VERSION_SIZE; i++) {
    version[i] = EEPROM.read(EEPROM_FIRMWARE_VERSION_OFFSET + i);
  }
  version[EEPROM_FIRMWARE_VERSION_SIZE] = '\0';

  // Validate: must start with a digit (e.g., "1.0.0")
  if (version[0] < '0' || version[0] > '9') {
    Serial.println("[OTA] No valid version in EEPROM, using default");
    return String(DEFAULT_FIRMWARE_VERSION);
  }

  String result = String(version);
  Serial.printf("[OTA] Loaded firmware version: %s\n", result.c_str());
  return result;
}

// ─── Parse Semantic Version ──────────────────────────────────
// Parses "major.minor.patch" into three integers.
// Returns true on success.
inline bool parseSemVer(const String &ver, int &major, int &minor, int &patch) {
  int firstDot = ver.indexOf('.');
  if (firstDot < 0)
    return false;

  int secondDot = ver.indexOf('.', firstDot + 1);
  if (secondDot < 0)
    return false;

  major = ver.substring(0, firstDot).toInt();
  minor = ver.substring(firstDot + 1, secondDot).toInt();
  patch = ver.substring(secondDot + 1).toInt();

  return true;
}

// ─── Compare Semantic Versions ───────────────────────────────
// Returns true if 'latest' is newer than 'current'.
inline bool isNewerVersion(const String &current, const String &latest) {
  int curMajor, curMinor, curPatch;
  int latMajor, latMinor, latPatch;

  if (!parseSemVer(current, curMajor, curMinor, curPatch)) {
    Serial.printf("[OTA] Failed to parse current version: %s\n",
                  current.c_str());
    return false;
  }

  if (!parseSemVer(latest, latMajor, latMinor, latPatch)) {
    Serial.printf("[OTA] Failed to parse latest version: %s\n", latest.c_str());
    return false;
  }

  Serial.printf("[OTA] Current: %d.%d.%d | Latest: %d.%d.%d\n", curMajor,
                curMinor, curPatch, latMajor, latMinor, latPatch);

  if (latMajor != curMajor)
    return latMajor > curMajor;
  if (latMinor != curMinor)
    return latMinor > curMinor;
  return latPatch > curPatch;
}

// ─── Check and Perform OTA Update ────────────────────────────
// Call this during setup() after WiFi is connected.
inline void checkAndPerformOTA() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi not connected, skipping OTA check");
    return;
  }

  String currentVersion = loadFirmwareVersion();
  String macAddress = WiFi.macAddress();

  Serial.println("[OTA] ─────────────────────────────────────");
  Serial.println("[OTA] Checking for firmware updates...");
  Serial.printf("[OTA] Device MAC: %s\n", macAddress.c_str());
  Serial.printf("[OTA] Current version: %s\n", currentVersion.c_str());

  // ── Step 1: Check latest version from server ──
  WiFiClient client;
  HTTPClient http;

  String versionUrl =
      String(OTA_VERSION_CHECK_URL) + "?macAddress=" + macAddress;

  Serial.printf("[OTA] Version check URL: %s\n", versionUrl.c_str());

  if (!http.begin(client, versionUrl)) {
    Serial.println("[OTA] Failed to connect to version server");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA] Version check failed, HTTP code: %d\n", httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  Serial.printf("[OTA] Server response: %s\n", payload.c_str());

  // ── Step 2: Parse JSON response ──
  StaticJsonDocument<256> doc;
  DeserializationError jsonErr = deserializeJson(doc, payload);
  if (jsonErr) {
    Serial.printf("[OTA] JSON parse error: %s\n", jsonErr.c_str());
    return;
  }

  // Extract version from JSON (expects {"version": "x.y.z"})
  const char *latestVersion = doc["data"];
  if (!latestVersion) {
    Serial.println("[OTA] No 'version' field in response");
    return;
  }

  String latestVersionStr = String(latestVersion);
  Serial.printf("[OTA] Latest version from server: %s\n",
                latestVersionStr.c_str());

  // ── Step 3: Compare versions ──
  if (!isNewerVersion(currentVersion, latestVersionStr)) {
    Serial.println("[OTA] Firmware is up to date ✓");
    Serial.println("[OTA] ─────────────────────────────────────");
    return;
  }

  // ── Step 4: Download and install firmware ──
  Serial.println("[OTA] *** New firmware available! ***");
  Serial.printf("[OTA] Updating: %s → %s\n", currentVersion.c_str(),
                latestVersionStr.c_str());

  String firmwareUrl =
      String(OTA_FIRMWARE_DOWNLOAD_URL) + "?macAddress=" + macAddress;

  Serial.printf("[OTA] Downloading firmware from: %s\n", firmwareUrl.c_str());

  // Configure HTTPUpdate
  ESPhttpUpdate.setLedPin(LED_BUILTIN, LOW);
  ESPhttpUpdate.rebootOnUpdate(false); // We handle reboot manually

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, firmwareUrl);

  switch (ret) {
  case HTTP_UPDATE_OK:
    Serial.println("[OTA] ★ Update successful! ★");
    // Save new version to EEPROM before restart
    saveFirmwareVersion(latestVersionStr);
    Serial.println("[OTA] New version saved to EEPROM");
    Serial.println("[OTA] Restarting device...");
    Serial.println("[OTA] ─────────────────────────────────────");
    delay(1000);
    ESP.restart();
    break;

  case HTTP_UPDATE_FAILED:
    Serial.printf("[OTA] Update FAILED (Error %d): %s\n",
                  ESPhttpUpdate.getLastError(),
                  ESPhttpUpdate.getLastErrorString().c_str());
    break;

  case HTTP_UPDATE_NO_UPDATES:
    Serial.println("[OTA] No update available from server");
    break;
  }

  Serial.println("[OTA] ─────────────────────────────────────");
}

#endif // OTA_UPDATE_LOGIC_H
