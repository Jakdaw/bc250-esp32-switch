#pragma once

#include <Arduino.h>

// MQTT publisher + Home Assistant MQTT discovery (PLAN.md Phase 5).
//
// Exposes one HA device made of three entities:
//   * switch        - the PSU on/off state (commands: ON / OFF / TOGGLE)
//   * binary_sensor - board up/down (TPMS1)
//   * sensor        - TPMS1 sense voltage in mV
//
// State topics are retained and republished on change and after every
// (re)connect; an LWT ("offline", retained) is registered at connect so HA
// marks the device unavailable if the broker or WiFi link drops. Discovery
// payloads are republished on connect and whenever the MQTT/device config
// changes (detected by diffing the live config against the applied snapshot).
//
// unique_id is "bc250-switch-<mac>", fixed for the life of the hardware so
// HA entities survive device renames.

// Start topics/callbacks and take the first config snapshot. Call once, after
// loadConfig().
void mqttBegin();
// Call from loop(): backoff reconnect, client.loop(), change detection,
// retained-state publishing. No-op when MQTT is disabled or unconfigured.
void mqttLoop();
// True while the broker connection is established.
bool mqttConnected();
