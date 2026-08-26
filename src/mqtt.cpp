#include "mqtt.h"
#include "board.h"
#include "config.h"
#include "power.h"
#include "wifi.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

#include <PubSubClient.h>

// Largest HA discovery packet is ~470 bytes (header + topic + 398-byte JSON);
// 1024 leaves headroom for long device names.
static const uint16_t MQTT_BUFFER_SIZE = 1024;

static WiFiClient   s_net;
static PubSubClient s_client(s_net);

// --- Topics (filled in mqttBegin) ---
static String s_uid;        // "bc250-switch-<mac>"
static String s_topicState;    // retained switch state ("ON"/"OFF")
static String s_topicCmd;      // commands in ("ON"/"OFF"/"TOGGLE")
static String s_topicAvail;    // retained availability ("online"/"offline", LWT)
static String s_topicBoard;    // retained board up/down ("ON"/"OFF")
static String s_topicSense;    // retained TPMS1 voltage (mV)
static String s_topicDiscSwitch;  // homeassistant/switch/<uid>/config
static String s_topicDiscBoard;   // homeassistant/binary_sensor/<uid>-board/config
static String s_topicDiscSense;   // homeassistant/sensor/<uid>-sense/config

// Discovery payloads, rebuilt when the device name changes.
static String s_discSwitch;
static String s_discBoard;
static String s_discSense;

// --- Connection / retry state ---
static bool          s_connected   = false;
static unsigned long s_lastAttempt = 0;
static unsigned long s_retryDelay  = MQTT_RETRY_START_MS;

// Last published values (change detection).
static bool          s_lastPubOn    = false;
static bool          s_lastPubBoard = false;
static unsigned long s_lastSensePub = 0;

// Applied-config snapshot (dashboard changes are detected by diffing).
static bool   s_appliedEnabled = false;
static String s_appliedHost;
static int    s_appliedPort = 0;
static String s_appliedUser;
static String s_appliedPass;
static String s_appliedName;

// --- Discovery ---

static void addDevice(JsonDocument &doc) {
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["name"]        = config.deviceName;
  dev["identifiers"].add(s_uid);
  dev["manufacturer"] = "BC250";
  dev["model"]        = "PSU switch";
}

// (Re)build the three retained discovery payloads from the current config.
static void buildDiscovery() {
  JsonDocument doc;

  doc.clear();
  doc["name"]               = config.deviceName;
  doc["unique_id"]          = s_uid;
  doc["command_topic"]      = s_topicCmd;
  doc["state_topic"]        = s_topicState;
  doc["availability_topic"] = s_topicAvail;
  doc["payload_on"]         = "ON";
  doc["payload_off"]        = "OFF";
  addDevice(doc);
  serializeJson(doc, s_discSwitch);

  doc.clear();
  doc["name"]         = "board up";
  doc["unique_id"]    = s_uid + "-board";
  doc["state_topic"]  = s_topicBoard;
  doc["payload_on"]   = "ON";
  doc["payload_off"]  = "OFF";
  doc["device_class"] = "power";
  addDevice(doc);
  serializeJson(doc, s_discBoard);

  doc.clear();
  doc["name"]                  = "sense voltage";
  doc["unique_id"]             = s_uid + "-sense";
  doc["state_topic"]           = s_topicSense;
  doc["unit_of_measurement"]   = "mV";
  doc["state_class"]           = "measurement";
  addDevice(doc);
  serializeJson(doc, s_discSense);
}

// --- Publishing ---

// Publish all retained state (switch, board, sense) and remember the values.
static void publishStateAll() {
  bool on = (getState() != STATE_OFF);  // BOOTING counts as ON (PSU asserted)
  s_client.publish(s_topicState.c_str(), on ? "ON" : "OFF", true);
  s_lastPubOn = on;

  bool up = isBoardUp();
  s_client.publish(s_topicBoard.c_str(), up ? "ON" : "OFF", true);
  s_lastPubBoard = up;

  char buf[12];
  snprintf(buf, sizeof buf, "%lu", (unsigned long)getBoardMv());
  s_client.publish(s_topicSense.c_str(), buf, true);
  s_lastSensePub = millis();
}

// Runs once per successful (re)connect: availability, discovery,
// resubscribe (subscriptions don't survive a broker-side reconnect), state.
// publish() fails SILENTLY (false return) when a payload exceeds the buffer,
// so check and log the results.
static void onConnected() {
  bool ok = true;
  ok &= s_client.publish(s_topicAvail.c_str(), "online", true);
  ok &= s_client.publish(s_topicDiscSwitch.c_str(), s_discSwitch.c_str(), true);
  ok &= s_client.publish(s_topicDiscBoard.c_str(), s_discBoard.c_str(), true);
  ok &= s_client.publish(s_topicDiscSense.c_str(), s_discSense.c_str(), true);
  s_client.subscribe(s_topicCmd.c_str());
  publishStateAll();
  if (!ok) {
    Serial.printf("[MQTT] WARNING: publish failed (buffer %u bytes; payloads "
                  "switch=%u board=%u sense=%u)\n", (unsigned)s_client.getBufferSize(),
                  (unsigned)s_discSwitch.length(), (unsigned)s_discBoard.length(),
                  (unsigned)s_discSense.length());
  } else {
    Serial.println("[MQTT] connected, discovery + state published");
  }
}

// Incoming command (runs on the loop task via s_client.loop()).
static void onMessage(char *topic, uint8_t *payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  cmd.toUpperCase();
  Serial.printf("[MQTT] command: %s\n", cmd.c_str());
  if (cmd == "ON") {
    requestPowerOn("MQTT");
  } else if (cmd == "OFF") {
    requestPowerOff("MQTT");
  } else if (cmd == "TOGGLE") {
    if (getState() == STATE_OFF) requestPowerOn("MQTT (toggle)");
    else                         requestPowerOff("MQTT (toggle)");
  }
}

// --- Config changes ---

// Drop the connection (announcing offline first) and adopt the current
// config as the new applied snapshot.
static void applyConfig() {
  if (s_connected) {
    // Clean disconnect would not fire the LWT, so publish it by hand first.
    s_client.publish(s_topicAvail.c_str(), "offline", true);
    s_client.disconnect();
    s_connected = false;
    Serial.println("[MQTT] config changed, reconnecting");
  }
  s_appliedEnabled = config.mqttEnabled;
  s_appliedHost    = config.mqttHost;
  s_appliedPort    = config.mqttPort;
  s_appliedUser    = config.mqttUser;
  s_appliedPass    = config.mqttPass;
  s_appliedName    = config.deviceName;
  s_retryDelay     = MQTT_RETRY_START_MS;
  s_lastAttempt    = 0;
  buildDiscovery();
}

// --- Public API ---

void mqttBegin() {
  // Stable unique id from the WiFi MAC: fixed for the hardware, so HA
  // entities survive device renames.
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char uid[40];
  snprintf(uid, sizeof uid, "bc250-switch-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  s_uid = uid;

  s_topicState        = "bc250/" + s_uid + "/state";
  s_topicCmd          = "bc250/" + s_uid + "/set";
  s_topicAvail        = "bc250/" + s_uid + "/availability";
  s_topicBoard        = "bc250/" + s_uid + "/board";
  s_topicSense        = "bc250/" + s_uid + "/sense";
  s_topicDiscSwitch   = "homeassistant/switch/" + s_uid + "/config";
  s_topicDiscBoard    = "homeassistant/binary_sensor/" + s_uid + "-board/config";
  s_topicDiscSense    = "homeassistant/sensor/" + s_uid + "-sense/config";

  // The library constructs its buffer at the default 256 bytes (that macro is
  // baked into the precompiled library, not overridable here), which is too
  // small for the ~400-byte HA discovery payloads -- publish() would silently
  // drop them. Grow the buffer at runtime so discovery fits.
  s_client.setBufferSize(MQTT_BUFFER_SIZE);

  s_client.setCallback(onMessage);
  applyConfig();
  Serial.printf("[MQTT] uid=%s enabled=%d host=%s port=%d buffer=%u\n",
                s_uid.c_str(), (int)config.mqttEnabled, config.mqttHost.c_str(),
                config.mqttPort, (unsigned)s_client.getBufferSize());
}

void mqttLoop() {
  unsigned long now = millis();

  // Dashboard changed the MQTT/device config -> re-apply (also resets backoff).
  if (config.mqttEnabled != s_appliedEnabled ||
      config.mqttHost    != s_appliedHost ||
      config.mqttPort    != s_appliedPort ||
      config.mqttUser    != s_appliedUser ||
      config.mqttPass    != s_appliedPass ||
      config.deviceName  != s_appliedName) {
    applyConfig();
  }

  if (!config.mqttEnabled || config.mqttHost.length() == 0) return;

  if (!s_connected) {
    // The broker is only reachable through the STA link (not the fallback AP).
    if (!wifiStaConnected()) return;
    if (now - s_lastAttempt < s_retryDelay) return;
    s_lastAttempt = now;

    s_client.setServer(config.mqttHost.c_str(), config.mqttPort);
    // LWT: retained "offline" on the availability topic (Q4: password only
    // used when a username is set).
    bool ok;
    if (config.mqttUser.length() > 0) {
      ok = s_client.connect(s_uid.c_str(),
                            config.mqttUser.c_str(),
                            config.mqttPass.c_str(),
                            s_topicAvail.c_str(), 1, true, "offline");
    } else {
      ok = s_client.connect(s_uid.c_str(), nullptr, nullptr,
                            s_topicAvail.c_str(), 1, true, "offline");
    }
    if (ok) {
      s_connected  = true;
      s_retryDelay = MQTT_RETRY_START_MS;
      onConnected();
    } else {
      s_retryDelay = min(s_retryDelay * 2, MQTT_RETRY_MAX_MS);
      Serial.printf("[MQTT] connect to %s:%d failed (rc=%d), retry in %lu ms\n",
                    config.mqttHost.c_str(), config.mqttPort,
                    s_client.state(), (unsigned long)s_retryDelay);
    }
    return;
  }

  if (!s_client.loop()) {
    s_connected  = false;
    s_retryDelay = MQTT_RETRY_START_MS;
    Serial.println("[MQTT] connection lost");
    return;
  }

  // Retained state: publish on change.
  bool on = (getState() != STATE_OFF);
  if (on != s_lastPubOn) {
    s_client.publish(s_topicState.c_str(), on ? "ON" : "OFF", true);
    s_lastPubOn = on;
  }
  bool up = isBoardUp();
  if (up != s_lastPubBoard) {
    s_client.publish(s_topicBoard.c_str(), up ? "ON" : "OFF", true);
    s_lastPubBoard = up;
  }
  if (now - s_lastSensePub >= MQTT_SENSE_PERIOD_MS) {
    char buf[12];
    snprintf(buf, sizeof buf, "%lu", (unsigned long)getBoardMv());
    s_client.publish(s_topicSense.c_str(), buf, true);
    s_lastSensePub = now;
  }
}

bool mqttConnected() {
  return s_connected && s_client.connected();
}
