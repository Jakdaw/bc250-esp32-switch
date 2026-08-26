#include <Arduino.h>
#include <NimBLEDevice.h>
#include "board.h"
#include "config.h"
#include "mqtt.h"
#include "power.h"
#include "web.h"
#include "wifi.h"

// The ESP32-C3 is permanently powered from the ATX connector (5VSB), so it runs
// independently of the PSU's main rails. It controls the SFX PSU through PS_ON#
// and watches the BC250's TPMS1 line to know whether the board itself is up.
//
// Control model
// -------------
//   * Asserting PS_ON# turns the PSU on; the BC250 is expected to power up from
//     applied power (BIOS "restore on AC power"), so PSU on == board boots.
//   * We have no wire to the board's power button, so "turning the board off"
//     means cutting PSU power via PS_ON#. This is a hard power-off, not a
//     graceful OS shutdown.
//   * If the board shuts itself down (e.g. OS shutdown), TPMS1 drops to 0 while
//     the PSU is still energized. We detect that and release PS_ON# so the PSU
//     follows the board down.
//   * A background BLE scan watches for the 8BitDo controller advertising; when
//     it appears while we're OFF, that wakes the machine just like a button tap.
//   * The device also joins the home WiFi as a client when credentials are
//     configured (see wifi.cpp); the setup SoftAP comes up as a fallback when
//     it has nothing to connect to or the connection fails.
//   * The web dashboard and MQTT (later phases) request power on/off through
//     the power.h API; requests are flagged and executed on the loop's clock.

static PowerState state = STATE_OFF;

// --- Button debounce / press tracking ---
static bool          buttonStable      = false;  // debounced "pressed" level
static bool          buttonLastRaw     = false;
static unsigned long buttonLastChange  = 0;
static unsigned long pressStart        = 0;
static bool          pressStartedOff   = false;  // press began while OFF
static bool          longPressFired    = false;
static bool          factoryFired      = false;

// --- Board-sense debounce ---
static bool          boardSenseStable  = false;  // debounced TPMS1 HIGH
static bool          boardSenseLastRaw = false;
static unsigned long boardSenseChange  = 0;

// --- BLE wake ---
// Written from the NimBLE scan task, read from loop(). 32-bit aligned scalars
// are atomic on the ESP32, so a plain volatile is enough here.
static volatile bool          bleSeenEver = false;
static volatile unsigned long bleLastSeen = 0;
static unsigned long          bleInhibitUntil = 0;  // wakes ignored until this

// --- Misc timers ---
static unsigned long bootStart    = 0;
static unsigned long lastHeartbeat = 0;

const char *stateName(PowerState s) {
  switch (s) {
    case STATE_OFF:     return "OFF";
    case STATE_BOOTING: return "BOOTING";
    case STATE_ON:      return "ON";
  }
  return "?";
}

static void setState(PowerState next) {
  if (next == state) return;
  Serial.printf("[STATE] %s -> %s\n", stateName(state), stateName(next));
  state = next;
}

// Raw (pre-time-debounce) board-up level, derived from the TPMS1 voltage with
// hysteresis so a signal hovering near the logic threshold doesn't chatter.
static bool senseLevel = false;

// Last averaged reading, exposed via getBoardMv().
static uint32_t s_lastSenseMv = 0;

static bool readBoardSense(uint32_t *outMv = nullptr) {
  // Average several reads to reject single-sample ADC/line spikes. Without this,
  // one stray spike past SENSE_HIGH_MV chatters the hysteresis and restarts the
  // board-off debounce, stalling shutdown detection (see SENSE_OVERSAMPLE).
  uint32_t acc = 0;
  for (int i = 0; i < SENSE_OVERSAMPLE; i++) {
    acc += analogReadMilliVolts(BOARD_SENSE);
  }
  uint32_t mv = acc / SENSE_OVERSAMPLE;
  s_lastSenseMv = mv;
  if (outMv) *outMv = mv;
  if (senseLevel) {
    if (mv < SENSE_LOW_MV) senseLevel = false;
  } else {
    if (mv > SENSE_HIGH_MV) senseLevel = true;
  }
  return senseLevel;
}

static void psuOn() {
  digitalWrite(PS_ON_PIN, PS_ON_ASSERT);
  Serial.println("[PSU ] PS_ON# asserted (LOW) -> PSU ON");
}

static void psuOff() {
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);
  Serial.println("[PSU ] PS_ON# released (high-Z) -> PSU OFF");
}

// Shared power-on path, used by both the button and the BLE wake. Takes the
// loop's `now` rather than calling millis() itself: the BOOTING timeout compares
// against the `now` cached at the top of normalLoop(), and a fresh millis() here
// can land a millisecond past it. Since the comparison is unsigned, bootStart >
// now makes (now - bootStart) underflow to a huge value and trip the timeout on
// the very next loop. Sharing one clock per iteration keeps the math monotonic.
static void powerOn(const char *reason, unsigned long now) {
  Serial.printf("[ACT ] %s -> powering on\n", reason);
  psuOn();
  bootStart = now;
  setState(STATE_BOOTING);
}

// Shared power-off path. Starts the BLE-wake cooldown so the controller's
// post-shutdown reconnect burst can't immediately wake us again. Takes `now` for
// the same single-clock-per-loop reason as powerOn().
static void powerOff(const char *reason, unsigned long now) {
  Serial.printf("[ACT ] %s -> powering off\n", reason);
  psuOff();
  bleInhibitUntil = now + BLE_WAKE_COOLDOWN_MS;
  setState(STATE_OFF);
}

// --- External power requests (web UI / MQTT, called from other tasks) ---
// Just flags; normalLoop() executes them on its own clock (see powerOn()'s
// single-clock note). 32-bit atomic flag access, no locking needed.
static volatile bool reqPowerOn  = false;
static volatile bool reqPowerOff = false;

void requestPowerOn(const char *source) {
  Serial.printf("[REQ ] %s -> power on\n", source);
  reqPowerOn = true;
}

void requestPowerOff(const char *source) {
  Serial.printf("[REQ ] %s -> power off\n", source);
  reqPowerOff = true;
}

// --- State getters (power.h) ---

PowerState getState() {
  return state;
}

uint32_t getBoardMv() {
  return s_lastSenseMv;
}

bool isBoardUp() {
  return boardSenseStable;
}

bool isBlePresent() {
  unsigned long now = millis();
  return bleSeenEver && (now - bleLastSeen) < BLE_PRESENCE_TIMEOUT_MS;
}

unsigned long getUptimeMs() {
  return millis();
}

// NimBLE scan task: just records that a bound controller was seen. The wake
// decision (edge-detect + state check) happens in loop(), off this task.
// Matches ANY MAC in config.wakeAddrs (lower-case compare via hasWakeAddr).
class WakeScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (hasWakeAddr(dev->getAddress().toString().c_str())) {
      bleLastSeen = millis();
      bleSeenEver = true;
    }
  }
};

static WakeScanCallbacks wakeScanCallbacks;
static bool              g_bleDisabledLogged = false;
static bool              g_bleScanStarted = false;

void startBleScan() {
  if (config.wakeAddrs.length() == 0) {
    if (!g_bleDisabledLogged) {
      g_bleDisabledLogged = true;
      Serial.println("[BLE ] no controllers bound; BLE wake disabled "
                     "(bind one from the web UI)");
    }
    return;
  }
  if (g_bleScanStarted) {
    NimBLEDevice::getScan()->stop();  // (re)start: replace any running scan
  }
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&wakeScanCallbacks, true);  // true = report duplicates
  scan->setActiveScan(false);  // passive: we only need the address, saves power
  scan->setInterval(160);      // ms
  scan->setWindow(80);         // ms (<= interval; ~50% duty)
  scan->start(0, false);       // 0 = scan continuously
  g_bleScanStarted = true;
  Serial.printf("[BLE ] scanning for controllers:\n%s",
                config.wakeAddrs.c_str());
}

void stopBleScan() {
  if (!g_bleScanStarted) return;
  NimBLEDevice::getScan()->stop();
  g_bleScanStarted = false;
}

// Wipe all config and reboot into the SoftAP setup network (PLAN.md D3).
static void doFactoryReset(const char *reason) {
  Serial.printf("[ACT ] %s -> factory reset\n", reason);
  factoryReset();
  delay(50);
  ESP.restart();
}

// Debounce a raw level into a stable level. Returns true if the stable value
// changed this call, writing the new value into *stable.
static bool debounce(bool raw, bool *stable, bool *lastRaw,
                     unsigned long *lastChange, unsigned long now,
                     unsigned long window) {
  if (raw != *lastRaw) {
    *lastRaw = raw;
    *lastChange = now;
  }
  if (raw != *stable && (now - *lastChange) >= window) {
    *stable = raw;
    return true;
  }
  return false;
}

static void normalBegin() {
  Serial.println("=== BC250 PSU controller ===");

  // PS_ON# open-drain, released by default so the PSU stays off at boot.
  pinMode(PS_ON_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(PS_ON_PIN, PS_ON_RELEASE);

  // Switch: GPIO6 = local ground, GPIO5 = sensed input with pull-up.
  pinMode(BUTTON_GND, OUTPUT);
  digitalWrite(BUTTON_GND, LOW);
  pinMode(BUTTON_SENSE, INPUT_PULLUP);

  // TPMS1 sense: read as ADC over the full 0-3.3V range.
  analogSetPinAttenuation(BOARD_SENSE, ADC_11db);

  // Seed debounced states from the current levels.
  buttonStable = buttonLastRaw = (digitalRead(BUTTON_SENSE) == LOW);
  boardSenseStable = boardSenseLastRaw = readBoardSense();

  // If the board is already up when the ESP (re)boots, adopt the ON state
  // rather than assuming OFF — keeps us in sync after an ESP-only reset.
  if (boardSenseStable) {
    psuOn();  // make sure PS_ON# matches reality
    setState(STATE_ON);
  }

  Serial.printf("[INIT] state=%s board=%s bound=%s\n",
                stateName(state), boardSenseStable ? "UP" : "DOWN",
                config.wakeAddrs.length() ? config.wakeAddrs.c_str() : "(none)");

  startBleScan();
}

void setup() {
  Serial.begin(115200);
  // Give USB-CDC a moment to enumerate so early logs aren't lost.
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {
    delay(10);
  }
  Serial.println();

  // Run at 80 MHz: plenty for this duty cycle and keeps idle current well
  // under the 160 MHz figure (see CPU_FREQ_HZ in board.h). The core started
  // us at F_CPU (160 MHz); setCpuFrequencyMhz() re-configures it.
  setCpuFrequencyMhz(CPU_FREQ_HZ / 1000000);
  Serial.printf("[INIT] CPU clock: %u MHz\n", getCpuFrequencyMhz());

  loadConfig();

  // One always-running mode (PLAN.md D1): the power state machine, WiFi and
  // the web server are always up. The fallback SoftAP (and with it the setup
  // experience) is handled by wifiLoop()/webLoop() — no separate setup mode.
  normalBegin();
  wifiBegin();
  webBegin();
  mqttBegin();
}

static void normalLoop() {
  unsigned long now = millis();

  // --- External power requests (web UI / MQTT) ---
  // Executed on this loop's clock; see the requestPowerOn/Off note. "on"
  // while ON/BOOTING is a no-op, "off" while BOOTING aborts the boot.
  if (reqPowerOn) {
    reqPowerOn = false;
    if (state == STATE_OFF) {
      powerOn("external request (on)", now);
    }
  }
  if (reqPowerOff) {
    reqPowerOff = false;
    if (state != STATE_OFF) {
      powerOff("external request (off)", now);
    }
  }

  // --- Sample & debounce inputs ---
  uint32_t senseMv;
  bool buttonRaw = (digitalRead(BUTTON_SENSE) == LOW);   // pressed == LOW
  bool boardRaw  = readBoardSense(&senseMv);             // board up (hysteresis)

  if (debounce(buttonRaw, &buttonStable, &buttonLastRaw,
               &buttonLastChange, now, DEBOUNCE_MS)) {
    if (buttonStable) {
      // Press began.
      pressStart = now;
      pressStartedOff = (state == STATE_OFF);
      longPressFired = false;
      factoryFired = false;
      Serial.println("[BTN ] pressed");
    } else {
      // Released. A short tap that began while OFF powers on (power-on is on
      // release so that a long hold from OFF can mean "factory reset" instead).
      unsigned long held = now - pressStart;
      Serial.printf("[BTN ] released after %lu ms\n", held);
      if (pressStartedOff && !factoryFired && state == STATE_OFF &&
          held < FACTORY_HOLD_MS) {
        powerOn("short press while OFF", now);
      }
    }
  }

  // --- Button holds ---
  if (buttonStable && pressStartedOff && !factoryFired &&
      (now - pressStart) >= FACTORY_HOLD_MS) {
    // Long hold from OFF -> factory reset (does not power the machine on).
    factoryFired = true;
    doFactoryReset("long hold (>8s) while OFF");
  }
  if (buttonStable && !pressStartedOff && !longPressFired && state == STATE_ON &&
      (now - pressStart) >= LONG_PRESS_MS) {
    // Long hold that began while ON -> force off.
    longPressFired = true;
    powerOff("long press (>5s) while ON", now);
  }

  // --- BLE controller wake ("machine follows controller") ---
  // While OFF, the controller being present powers the machine on. The guard
  // window after a power-off lets you switch the controller off first (so it
  // goes absent and the machine stays down) and rides out the controller's
  // reconnect-advertising burst at shutdown.
  bool blePresent =
      bleSeenEver && (now - bleLastSeen) < BLE_PRESENCE_TIMEOUT_MS;
  bool bleInhibited = (int32_t)(bleInhibitUntil - now) > 0;
  if (state == STATE_OFF && blePresent && !bleInhibited) {
    powerOn("controller present (BLE)", now);
  }

  bool boardChanged = debounce(boardRaw, &boardSenseStable, &boardSenseLastRaw,
                               &boardSenseChange, now,
                               state == STATE_BOOTING ? DEBOUNCE_MS
                                                      : BOARD_OFF_DEBOUNCE_MS);

  // --- State-driven board-sense handling ---
  switch (state) {
    case STATE_BOOTING:
      if (boardSenseStable) {
        Serial.println("[ACT ] TPMS1 HIGH -> board is up");
        setState(STATE_ON);
      } else if ((now - bootStart) >= BOOT_TIMEOUT_MS) {
        powerOff("boot timed out, board never signalled UP", now);
      }
      break;

    case STATE_ON:
      // Board dropped TPMS1 on its own (OS shutdown / crash) -> follow it down.
      if (boardChanged && !boardSenseStable) {
        powerOff("TPMS1 LOW while ON, board shut down", now);
      }
      break;

    case STATE_OFF:
    default:
      break;
  }

  // --- Heartbeat ---
  if (now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    Serial.printf("[HB  ] state=%s board=%s btn=%s ble=%s | sense: %umV (%s)\n",
                  stateName(state),
                  boardSenseStable ? "UP" : "DOWN",
                  buttonStable ? "down" : "up",
                  blePresent ? "present" : "absent",
                  senseMv, boardRaw ? "high" : "low");
  }
}

void loop() {
  wifiLoop();
  mqttLoop();
  webLoop();
  normalLoop();
  // Only the loop task idles here — the WiFi stack, NimBLE scan and the async
  // web server each run in their own tasks and are unaffected (see
  // LOOP_IDLE_MS in board.h).
  delay(LOOP_IDLE_MS);
}
