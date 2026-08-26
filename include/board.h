#pragma once

//*******  Pin definitions  ***************
//
// BC250 PSU controller wiring:
//
//   ESP32-C3                      External
//   --------                      --------
//   GPIO5  (BUTTON_SENSE)  <-----> momentary switch terminal A
//   GPIO6  (BUTTON_GND)    <-----> momentary switch terminal B
//   GPIO4  (PS_ON_PIN)     <-----> ATX PS_ON# (green wire, active LOW)
//   GPIO3  (BOARD_SENSE)   <-----> BC250 TPMS1 pin 9 (3.3V = board on)
//
// The switch bridges GPIO5 and GPIO6. GPIO6 is driven LOW to act as a local
// ground, and GPIO5 is read with an internal pull-up: pressed reads LOW.
const int BUTTON_SENSE = 5;
const int BUTTON_GND   = 6;

// ATX PS_ON# is active LOW and idles at ~5V (pulled up inside the PSU).
// Driven as OPEN-DRAIN so we never push 3.3V against the PSU's 5V pull-up:
//   LOW  -> sink to GND -> PSU on
//   HIGH -> high-impedance -> PSU pull-up wins -> PSU off
const int PS_ON_PIN = 4;

// BC250 TPMS1 (pin 9): reads ~3.3V while the board is powered/booted, 0 when
// off. In practice it's a higher-impedance source that settles near ~2.9V and
// hovers close to the ESP's digital logic threshold, so digitalRead() flickers.
// We read it as an ADC voltage with hysteresis instead (see thresholds below).
const int BOARD_SENSE = 3;

// Hysteresis thresholds for the analog board-sense reading. The gap between
// them keeps a noisy signal sitting near the threshold from chattering:
//   reading rises above HIGH -> treat as "board up"
//   reading falls below LOW  -> treat as "board down"
//   in between               -> hold previous state
const int SENSE_HIGH_MV = 2000;
const int SENSE_LOW_MV  = 800;

// TPMS1 is high-impedance and the ESP32 single-shot ADC is noisy, so an isolated
// analogReadMilliVolts() can spike hundreds of mV above the true level. Once the
// board powers off the line floats near 0V but still throws the occasional spike
// past SENSE_HIGH_MV. A single such spike flips the hysteresis HIGH for one loop,
// which restarts the BOARD_OFF_DEBOUNCE_MS countdown -> shutdown detection stalls
// for an unbounded, random time. Averaging this many samples per reading is a
// low-pass that keeps a lone spike from ever crossing a threshold.
const int SENSE_OVERSAMPLE = 16;

//*******  Logic levels  ***************

const int PS_ON_ASSERT  = LOW;   // PSU on
const int PS_ON_RELEASE = HIGH;  // PSU off (open-drain -> high-Z)

//*******  Timing (milliseconds)  ***************

// Switch debounce window.
const unsigned long DEBOUNCE_MS = 30;

// Hold the button this long while the board is ON to force it off.
const unsigned long LONG_PRESS_MS = 5000;

// Hold the button this long while OFF to FACTORY RESET: wipe all config
// (password, WiFi, MQTT, bound controllers) and reboot into the SoftAP setup
// network (PLAN.md D3). Longer than LONG_PRESS_MS and only armed for presses
// that begin while OFF, so it never collides with force-off.
const unsigned long FACTORY_HOLD_MS = 8000;

// TPMS1 must stay LOW continuously for this long before we treat the board as
// having shut itself down. Filters out brief dips/transients during boot/reset.
const unsigned long BOARD_OFF_DEBOUNCE_MS = 1500;

// How long to wait for TPMS1 to go HIGH after asserting PS_ON#. If the board
// hasn't signalled UP by then we assume the boot failed, release the PSU and
// return to idle (OFF).
const unsigned long BOOT_TIMEOUT_MS = 10000;

// Periodic heartbeat log interval. 30 s keeps the serial log readable without
// affecting anything (logging has no power impact once the PSU is unplugged).
const unsigned long HEARTBEAT_MS = 30000;

//*******  WiFi setup portal  ***************

// SoftAP name shown when the device is in setup mode (open network).
const char *const AP_SSID = "BC250 Switch Setup";

// WiFi TX power for the SoftAP. These ESP32-C3 mini boards have an RF/power
// design flaw (arduino-esp32 #6551): at full power the AP emits no usable
// beacons, so the portal is invisible. A low value fixes it. WIFI_POWER_8_5dBm
// is confirmed working on this board.
#define AP_TX_POWER WIFI_POWER_8_5dBm

//*******  Networking  ***************

// mDNS name the device registers on the LAN, so it is reachable at
// http://bc250-switch.local whenever it is connected to home WiFi.
const char *const MDNS_NAME = "bc250-switch";

// If STA credentials are set but we've been without a connection for this
// long, bring up the fallback SoftAP (STA+AP) so the dashboard stays reachable
// and the WiFi can be reconfigured without a button hold. The SoftAP drops
// again once STA connects.
const unsigned long WIFI_FALLBACK_AFTER_MS = 30000;

//*******  CPU / power  ***************

// CPU clock. 80 MHz is plenty for this duty cycle and keeps idle current well
// under the 160 MHz figure; the slight extra WiFi/BLE handler latency is
// irrelevant here.
const uint32_t CPU_FREQ_HZ = 80000000;

// Idle delay at the end of loop(). Only the loop task sleeps here — the WiFi
// stack, the NimBLE scan task, and the async web server each run in their own
// tasks and are unaffected.
const unsigned long LOOP_IDLE_MS = 5;

//*******  BLE wake  ***************

// Bound controller BLE MACs are configured via the web UI and stored in NVS
// (see config.h: config.wakeAddrs). When the machine is OFF and ANY of them is
// advertising, we power on ("machine follows controller").

// The controller counts as "present" while it has been seen within this window.
// While OFF, presence => the machine powers on ("machine follows controller").
const unsigned long BLE_PRESENCE_TIMEOUT_MS = 4000;

// Guard window after any power-off during which BLE presence is ignored. This is
// your chance to also switch the controller off (it then goes absent and the
// machine stays down). If you leave the controller on, once this elapses the
// machine follows it back on. It also rides out the brief reconnect-advertising
// burst the controller emits when it loses its host at shutdown.
const unsigned long BLE_WAKE_COOLDOWN_MS = 15000;

//*******  BLE device discovery (web picker)  ***************
//
// The dashboard's "bind controller" picker runs an ACTIVE scan (it needs names,
// so it can't be passive like the wake scan). NimBLE allows one scan at a time,
// so while the picker is in use it replaces the passive wake scan and is
// auto-ended when the UI stops polling; the wake scan resumes afterwards.

// Picker scan cadence. Active scans on the C3 share the single radio with WiFi,
// so keep the duty low (window <= interval).
const uint16_t PICKER_SCAN_INTERVAL_MS = 120;
const uint16_t PICKER_SCAN_WINDOW_MS   = 40;

// End the picker scan this long after the last /api/ble/devices request, then
// resume the passive wake scan.
const unsigned long PICKER_IDLE_MS = 15000;

//*******  MQTT / Home Assistant  ***************

// Broker reconnect backoff: first retry after this long, doubling per failure
// up to the cap. Reset on a successful connect and on any config change.
const unsigned long MQTT_RETRY_START_MS = 5000;
const unsigned long MQTT_RETRY_MAX_MS   = 60000;

// Retained sense-voltage republish period while connected (mV is fairly stable
// after oversampling, but this keeps the HA gauge fresh without spamming).
const unsigned long MQTT_SENSE_PERIOD_MS = 5000;
