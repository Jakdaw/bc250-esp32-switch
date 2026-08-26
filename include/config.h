#pragma once

#include <Arduino.h>

// Persistent configuration, stored in NVS via the Preferences library.
//
//   passHash    - SHA-256 hex of the web UI password ("" => not set yet).
//   wakeAddrs   - bound controller BLE MACs, one per line, lower-case "aa:bb:..".
//                 Any of them waking the machine is allowed; empty = BLE wake off.
//   wifiSsid    - home network SSID the device joins as a client ("" => none).
//   wifiPass    - home network password (plaintext; NVS is unencrypted anyway).
//   mqttEnabled - publish to the Home Assistant MQTT broker.
//   mqttHost    - broker IP or hostname.
//   mqttPort    - broker port (default 1883).
//   mqttUser    - optional username (password used only if this is set).
//   mqttPass    - broker password.
//   deviceName  - HA device name + MQTT topic base (default "bc250-switch").
struct Config {
  String passHash;
  String wakeAddrs;
  String wifiSsid;
  String wifiPass;
  bool   mqttEnabled;
  String mqttHost;
  int    mqttPort;
  String mqttUser;
  String mqttPass;
  String deviceName;
};

extern Config config;

void loadConfig();

// --- Wake-controller list (newline-separated MACs) ---

// True if `addr` is already bound (case-insensitive).
bool hasWakeAddr(const String &addr);
// Bind a controller. Invalid/unrecognised addresses are ignored; duplicates
// are a no-op.
void addWakeAddr(const String &addr);
// Unbind a controller. A no-op if it isn't bound.
void removeWakeAddr(const String &addr);

// --- Web UI password ---
void setPassHash(const String &hash);

// --- WiFi client (STA) credentials ---
void setWifi(const String &ssid, const String &pass);

// --- MQTT / Home Assistant ---
void setMqtt(bool enabled, const String &host, int port,
             const String &user, const String &pass);
void setDeviceName(const String &name);

// Wipe every setting back to factory defaults and clear the NVS namespace.
// Callers reboot; a fresh device then boots straight into the SoftAP setup.
void factoryReset();
