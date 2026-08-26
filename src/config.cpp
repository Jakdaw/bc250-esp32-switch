#include "config.h"
#include <Preferences.h>

Config config;

static Preferences prefs;
static const char *NS = "bc250";

static const int    DEFAULT_MQTT_PORT   = 1883;
static const char *const DEFAULT_DEVICE_NAME = "bc250-switch";

void loadConfig() {
  prefs.begin(NS, true);  // read-only
  config.passHash    = prefs.getString("passHash", "");
  config.wakeAddrs   = prefs.getString("wakeAddrs", "");
  config.wifiSsid    = prefs.getString("wifiSsid", "");
  config.wifiPass    = prefs.getString("wifiPass", "");
  config.mqttEnabled = prefs.getBool("mqttEnabled", false);
  config.mqttHost    = prefs.getString("mqttHost", "");
  config.mqttPort    = prefs.getInt("mqttPort", DEFAULT_MQTT_PORT);
  config.mqttUser    = prefs.getString("mqttUser", "");
  config.mqttPass    = prefs.getString("mqttPass", "");
  config.deviceName  = prefs.getString("deviceName", DEFAULT_DEVICE_NAME);
  // Legacy single-MAC key from older firmware, folded in below.
  String legacyWake = prefs.getString("wakeAddr", "");
  prefs.end();

  // One-time migration: fold the legacy single wakeAddr into wakeAddrs and
  // drop the old key so the new layout is the only source of truth.
  if (config.wakeAddrs.length() == 0 && legacyWake.length() > 0) {
    config.wakeAddrs = legacyWake;
    prefs.begin(NS, false);
    prefs.putString("wakeAddrs", config.wakeAddrs);
    prefs.remove("wakeAddr");
    prefs.end();
  }
}

static void putString(const char *key, const String &val) {
  prefs.begin(NS, false);
  prefs.putString(key, val);
  prefs.end();
}

static void putInt(const char *key, int val) {
  prefs.begin(NS, false);
  prefs.putInt(key, val);
  prefs.end();
}

static void putBool(const char *key, bool val) {
  prefs.begin(NS, false);
  prefs.putBool(key, val);
  prefs.end();
}

// --- MAC handling ---

static bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// Normalise a MAC for storage/lookup: trim, lower-case. Writes the result to
// `out`; returns false if it is not a 17-char "aa:bb:cc:dd:ee:ff" address.
static bool normMac(const String &in, String &out) {
  String s = in;
  s.trim();
  s.toLowerCase();
  if (s.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) {
      if (s[i] != ':') return false;
    } else if (!isHexChar(s[i])) {
      return false;
    }
  }
  out = s;
  return true;
}

// Copy the i-th line of a "\n"-separated string ("" when past the end).
static String lineAt(const String &s, int i) {
  int start = 0;
  for (int n = 0; n <= i; n++) {
    int nl = s.indexOf('\n', start);
    if (nl < 0) {
      return (n == i) ? s.substring(start) : "";
    }
    if (n == i) return s.substring(start, nl);
    start = nl + 1;
  }
  return "";
}

bool hasWakeAddr(const String &addr) {
  String a;
  if (!normMac(addr, a)) return false;
  for (int i = 0; ; i++) {
    String line = lineAt(config.wakeAddrs, i);
    if (line.length() == 0) return false;
    if (line == a) return true;
  }
}

void addWakeAddr(const String &addr) {
  String a;
  if (!normMac(addr, a)) return;
  if (hasWakeAddr(a)) return;
  if (config.wakeAddrs.length() > 0) config.wakeAddrs += '\n';
  config.wakeAddrs += a;
  putString("wakeAddrs", config.wakeAddrs);
}

void removeWakeAddr(const String &addr) {
  String a;
  if (!normMac(addr, a)) return;

  String out;
  bool found = false;
  int start = 0;
  for ( ; ; ) {
    int nl = config.wakeAddrs.indexOf('\n', start);
    String line = (nl < 0) ? config.wakeAddrs.substring(start)
                           : config.wakeAddrs.substring(start, nl);
    if (line.length() > 0) {
      if (line == a) {
        found = true;
      } else {
        if (out.length() > 0) out += '\n';
        out += line;
      }
    }
    if (nl < 0) break;
    start = nl + 1;
  }
  if (found) putString("wakeAddrs", out);
}

void setPassHash(const String &hash) {
  config.passHash = hash;
  putString("passHash", hash);
}

void setWifi(const String &ssid, const String &pass) {
  config.wifiSsid = ssid;
  config.wifiPass = pass;
  putString("wifiSsid", ssid);
  putString("wifiPass", pass);
}

void setMqtt(bool enabled, const String &host, int port,
             const String &user, const String &pass) {
  config.mqttEnabled = enabled;
  config.mqttHost    = host;
  config.mqttPort    = port;
  config.mqttUser    = user;
  config.mqttPass    = pass;
  putBool("mqttEnabled", enabled);
  putString("mqttHost", host);
  putInt("mqttPort", port);
  putString("mqttUser", user);
  putString("mqttPass", pass);
}

void setDeviceName(const String &name) {
  config.deviceName = name;
  putString("deviceName", name);
}

void factoryReset() {
  config.passHash    = "";
  config.wakeAddrs   = "";
  config.wifiSsid    = "";
  config.wifiPass    = "";
  config.mqttEnabled = false;
  config.mqttHost    = "";
  config.mqttPort    = DEFAULT_MQTT_PORT;
  config.mqttUser    = "";
  config.mqttPass    = "";
  config.deviceName  = DEFAULT_DEVICE_NAME;
  prefs.begin(NS, false);
  prefs.clear();  // wipes the whole namespace, legacy keys included
  prefs.end();
}
