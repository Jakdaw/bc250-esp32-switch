#pragma once

#include <Arduino.h>

// Power control & state, owned by the state machine in main.cpp. The web UI,
// MQTT and BLE wake all funnel through this small API instead of reaching into
// loop internals.

// PSU/board power state machine.
enum PowerState {
  STATE_OFF,      // PSU released, board down
  STATE_BOOTING,  // PSU asserted, waiting for TPMS1 to go HIGH
  STATE_ON,       // PSU asserted, board up (TPMS1 HIGH)
};

// Request power on/off from another task (web UI / MQTT). The request is
// flagged and executed on the next loop() pass on the loop's clock, so the
// state machine and the BOOTING timeout keep sharing one time base. "on"
// while ON/BOOTING is a no-op; "off" while BOOTING aborts the boot.
void requestPowerOn(const char *source);
void requestPowerOff(const char *source);

PowerState getState();
const char *stateName(PowerState s);

// Last averaged TPMS1 reading in mV (board sense, see board.h).
uint32_t getBoardMv();
// Debounced board-up level.
bool isBoardUp();
// A bound controller has been seen within the presence window.
bool isBlePresent();
// Time since boot in ms.
unsigned long getUptimeMs();

// (Re)start the passive BLE wake scan, matching any MAC in config.wakeAddrs.
// An empty list disables BLE wake (logged once) and skips NimBLE init.
void startBleScan();
// Stop the passive wake scan (no-op if it isn't running).
void stopBleScan();
