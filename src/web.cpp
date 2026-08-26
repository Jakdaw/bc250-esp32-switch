#include "web.h"

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <esp_random.h>
#include "mbedtls/sha256.h"

#include "board.h"
#include "config.h"
#include "mqtt.h"
#include "power.h"
#include "wifi.h"

static const size_t MIN_PASSWORD_LEN = 6;

static DNSServer    dnsServer;
static AsyncWebServer server(80);

// Single-session bearer token, handed out on password set / login and required
// by the protected endpoints. Regenerated each set/login; "" means no session.
static String       g_token;

// Deferred actions: run on the loop task (or after a delay) so the HTTP
// response can flush first / WiFi calls happen off the web task.
static unsigned long g_rebootAt         = 0;
static bool          g_wifiApplyPending = false;

// --- BLE device discovery (active scan, accumulated for the picker) ---
// NimBLE runs one scan at a time, so while the picker is in use it replaces
// the passive wake scan; endPickerScan() resumes it.
struct BleDev {
  char addr[18];
  char name[33];
  int  rssi;
  bool used;
};
static const int    MAX_DEVS = 48;
static BleDev       devs[MAX_DEVS];
static portMUX_TYPE devsMux = portMUX_INITIALIZER_UNLOCKED;

class PickerScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *d) override {
    std::string addrStr = d->getAddress().toString();
    const char *addr = addrStr.c_str();
    std::string name = d->getName();
    int rssi = d->getRSSI();

    portENTER_CRITICAL(&devsMux);
    int idx = -1, free = -1;
    for (int i = 0; i < MAX_DEVS; i++) {
      if (devs[i].used) {
        if (strcmp(devs[i].addr, addr) == 0) { idx = i; break; }
      } else if (free < 0) {
        free = i;
      }
    }
    if (idx < 0 && free >= 0) {
      idx = free;
      devs[idx].used = true;
      strncpy(devs[idx].addr, addr, sizeof(devs[idx].addr) - 1);
      devs[idx].addr[sizeof(devs[idx].addr) - 1] = 0;
      devs[idx].name[0] = 0;
    }
    if (idx >= 0) {
      devs[idx].rssi = rssi;
      if (!name.empty()) {
        strncpy(devs[idx].name, name.c_str(), sizeof(devs[idx].name) - 1);
        devs[idx].name[sizeof(devs[idx].name) - 1] = 0;
      }
    }
    portEXIT_CRITICAL(&devsMux);
  }
};
static PickerScanCallbacks pickerCallbacks;

static bool          g_pickerActive      = false;
static unsigned long g_pickerLastRequest = 0;

// --- Helpers ---

static String sha256hex(const String &s) {
  uint8_t out[32];
  mbedtls_sha256((const unsigned char *)s.c_str(), s.length(), out, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", out[i]);
  hex[64] = 0;
  return String(hex);
}

static String makeToken() {
  char buf[33];
  for (int i = 0; i < 4; i++) sprintf(buf + i * 8, "%08x", (unsigned)esp_random());
  buf[32] = 0;
  return String(buf);
}

static bool authed(AsyncWebServerRequest *req) {
  if (g_token.isEmpty()) return false;
  if (!req->hasHeader("X-Auth-Token")) return false;
  return req->header("X-Auth-Token") == g_token;
}

static void sendJsonError(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

static void serveIndex(AsyncWebServerRequest *req) {
  if (SPIFFS.exists("/index.html")) {
    req->send(SPIFFS, "/index.html", "text/html");
  } else {
    req->send(200, "text/html",
              "<h1>BC250 Switch</h1><p>Filesystem image missing. Flash it with "
              "<code>pio run -t uploadfs</code>.</p>");
  }
}

static bool validMac(const String &s) {
  if (s.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    char c = s[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    if ((i % 3 == 2) ? (c != ':') : !hex) return false;
  }
  return true;
}

// --- Picker scan control ---

static void startPickerScan() {
  if (g_pickerActive) return;
  stopBleScan();  // pause the passive wake scan (one scan at a time)
  NimBLEDevice::init("");
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&pickerCallbacks, true);
  scan->setActiveScan(true);
  // Low duty: the C3 shares its single radio with WiFi (see board.h).
  scan->setInterval(PICKER_SCAN_INTERVAL_MS);
  scan->setWindow(PICKER_SCAN_WINDOW_MS);
  scan->start(0, false);
  g_pickerActive = true;
  Serial.println("[WEB ] picker scan started (wake scan paused)");
}

static void endPickerScan() {
  if (!g_pickerActive) return;
  NimBLEDevice::getScan()->stop();
  g_pickerActive = false;
  g_pickerLastRequest = 0;
  startBleScan();  // resume the wake scan (no-op if no controllers bound)
  Serial.println("[WEB ] picker scan ended, wake scan resumed");
}

// --- Endpoint handlers ---

// Public on purpose: the page uses it to know whether to ask for a password.
static void handleStatus(AsyncWebServerRequest *req) {
  JsonDocument doc;
  doc["passwordSet"] = config.passHash.length() > 0;
  doc["state"]       = stateName(getState());
  doc["boardMv"]     = getBoardMv();
  doc["boardUp"]     = isBoardUp();
  doc["blePresent"]  = isBlePresent();

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["connected"] = wifiStaConnected();
  wifi["ssid"]      = wifiStaConnected() ? WiFi.SSID().c_str() : "";
  wifi["ip"]        = wifiStaConnected() ? WiFi.localIP().toString().c_str() : "";
  wifi["rssi"]      = wifiStaConnected() ? (int)WiFi.RSSI() : 0;
  wifi["onSetupAp"] = wifiOnSetupAp();

  JsonObject mqtt = doc["mqtt"].to<JsonObject>();
  mqtt["connected"] = mqttConnected();
  mqtt["host"]      = config.mqttHost;

  JsonArray addrs = doc["wakeAddrs"].to<JsonArray>();
  int start = 0;
  for (;;) {
    int nl = config.wakeAddrs.indexOf('\n', start);
    String line = (nl < 0) ? config.wakeAddrs.substring(start)
                           : config.wakeAddrs.substring(start, nl);
    if (line.length() > 0) addrs.add(line);
    if (nl < 0) break;
    start = nl + 1;
  }

  doc["deviceName"] = config.deviceName;
  doc["uptimeMs"]   = getUptimeMs();

  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handlePower(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String action = o["action"] | "";
  if (action == "on") {
    requestPowerOn("web");
  } else if (action == "off") {
    requestPowerOff("web");
  } else {
    sendJsonError(req, 400, "invalid action");
    return;
  }
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleBleAdd(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String addr = o["addr"] | "";
  addr.toLowerCase();
  if (!validMac(addr)) {
    sendJsonError(req, 400, "invalid address");
    return;
  }
  bool wasEmpty = (config.wakeAddrs.length() == 0);
  addWakeAddr(addr);
  Serial.printf("[WEB ] bound controller %s\n", addr.c_str());
  // First controller: bring up the wake scan (skipped at boot when empty).
  if (wasEmpty && !g_pickerActive) startBleScan();
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleBleRemove(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  String addr = o["addr"] | "";
  addr.toLowerCase();
  if (!validMac(addr)) {
    sendJsonError(req, 400, "invalid address");
    return;
  }
  removeWakeAddr(addr);
  Serial.printf("[WEB ] unbound controller %s\n", addr.c_str());
  // Last controller gone: drop the wake scan (empty list = BLE wake off).
  if (config.wakeAddrs.length() == 0 && !g_pickerActive) stopBleScan();
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleWifi(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  setWifi(o["ssid"] | "", o["pass"] | "");
  // Applied on the loop task after this response flushes (a bad SSID would
  // otherwise kill the very connection carrying this reply).
  g_wifiApplyPending = true;
  Serial.println("[WEB ] wifi credentials saved, applying live");
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleMqtt(AsyncWebServerRequest *req, JsonVariant &json) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  JsonObject o = json.as<JsonObject>();
  int port = o["port"] | 1883;
  if (port < 1 || port > 65535) port = 1883;
  setMqtt(o["enabled"] | false, o["host"] | "", port,
          o["user"] | "", o["pass"] | "");
  String name = o["name"] | "";
  if (name.length() > 0) setDeviceName(name);
  Serial.printf("[WEB ] mqtt config saved (enabled=%d host=%s port=%d)\n",
                config.mqttEnabled, config.mqttHost.c_str(), config.mqttPort);
  req->send(200, "application/json", "{\"ok\":true}");
}

static void handleFactory(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }
  factoryReset();
  req->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[WEB ] factory reset requested, rebooting");
  g_rebootAt = millis() + 800;
}

static void handleBleDevices(AsyncWebServerRequest *req) {
  if (!authed(req)) { sendJsonError(req, 401, "unauthorized"); return; }

  // Keep the (time-boxed) picker scan alive for as long as the UI polls.
  g_pickerLastRequest = millis();
  if (!g_pickerActive) startPickerScan();

  // Snapshot under the lock, then build JSON without holding it.
  BleDev snap[MAX_DEVS];
  portENTER_CRITICAL(&devsMux);
  memcpy(snap, devs, sizeof(devs));
  portEXIT_CRITICAL(&devsMux);

  JsonDocument doc;
  JsonArray arr = doc["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVS; i++) {
    if (!snap[i].used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["addr"] = snap[i].addr;
    o["name"] = snap[i].name;
    o["rssi"] = snap[i].rssi;
  }
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handlePassword(AsyncWebServerRequest *req, JsonVariant &json) {
  JsonDocument doc;
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (pw.length() < MIN_PASSWORD_LEN) {
    sendJsonError(req, 400, "password too short");
    return;
  }
  // Setting the first password is open; changing an existing one needs a session.
  if (config.passHash.length() > 0 && !authed(req)) {
    sendJsonError(req, 401, "unauthorized");
    return;
  }
  setPassHash(sha256hex(pw));
  g_token = makeToken();
  Serial.println("[WEB ] password set");

  doc["token"] = g_token;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

static void handleLogin(AsyncWebServerRequest *req, JsonVariant &json) {
  if (config.passHash.isEmpty()) {
    sendJsonError(req, 400, "no password set");
    return;
  }
  JsonObject o = json.as<JsonObject>();
  String pw = o["password"] | "";
  if (sha256hex(pw) != config.passHash) {
    sendJsonError(req, 401, "wrong password");
    return;
  }
  g_token = makeToken();
  JsonDocument doc;
  doc["token"] = g_token;
  String out;
  serializeJson(doc, out);
  req->send(200, "application/json", out);
}

// --- Lifecycle ---

void webBegin() {
  Serial.println("=== BC250 PSU controller : WEB ===");

  if (!SPIFFS.begin(true)) {
    Serial.println("[WEB ] WARNING: SPIFFS mount failed");
  }

  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/ble/devices", HTTP_GET, handleBleDevices);
  server.on("/api/factory", HTTP_POST, handleFactory);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/power", handlePower));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/ble/add", handleBleAdd));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/ble/remove", handleBleRemove));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/wifi", handleWifi));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mqtt", handleMqtt));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/password", handlePassword));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", handleLogin));

  server.on("/", HTTP_GET, serveIndex);
  server.onNotFound([](AsyncWebServerRequest *req) {
    if (req->url().startsWith("/api/")) {
      sendJsonError(req, 404, "not found");
    } else {
      serveIndex(req);  // SPA catch-all (also the captive-portal reply)
    }
  });

  server.begin();
  Serial.printf("[WEB ] web server up (http://192.168.4.1 or http://%s.local)\n",
                MDNS_NAME);
}

void webLoop() {
  // Captive-portal DNS only while the fallback SoftAP is up. (DNS replies go
  // through an AsyncUDP callback in this core, so no per-loop processing.)
  if (wifiOnSetupAp()) {
    if (!dnsServer.isUp()) {
      dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
      dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
      Serial.println("[WEB ] captive DNS up (fallback SoftAP)");
    }
  } else if (dnsServer.isUp()) {
    dnsServer.stop();
  }

  // Time-box the picker scan, then resume the passive wake scan.
  if (g_pickerActive && (millis() - g_pickerLastRequest) > PICKER_IDLE_MS) {
    endPickerScan();
  }

  // Live WiFi credential apply, on the loop task (see handleWifi).
  if (g_wifiApplyPending) {
    g_wifiApplyPending = false;
    wifiApplyCredentials();
  }

  // Deferred reboot (so the HTTP response can flush before we reboot).
  if (g_rebootAt && (int32_t)(millis() - g_rebootAt) >= 0) {
    ESP.restart();
  }
}
