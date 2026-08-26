#include "wifi.h"
#include "board.h"
#include "config.h"

#include <WiFi.h>
#include <ESPmDNS.h>

// These C3 mini boards have an RF/power design flaw (arduino-esp32 #6551): at
// full TX power the SoftAP emits no usable beacons, so the fallback AP runs at
// AP_TX_POWER (see board.h) and we restore the previous power when it drops.
// While the AP is up in STA+AP mode the STA shares that lower power, which is
// acceptable: the AP is only up while the STA is already failing or there is
// nothing to connect to.

static const unsigned long RECONNECT_BASE_MS = 15000;
static const unsigned long RECONNECT_MAX_MS  = 60000;

static bool          g_apUp          = false;
static bool          g_wasConnected  = false;
static unsigned long g_downSince     = 0;  // millis() when STA went down (0 = up)
static unsigned long g_lastAttempt   = 0;
static unsigned long g_retryInterval = RECONNECT_BASE_MS;
static wifi_power_t  g_defaultTxPower = WIFI_POWER_20_5dBm;

static void startFallbackAp() {
  if (!WiFi.softAP(AP_SSID)) {
    Serial.println("[WIFI] WARNING: fallback SoftAP failed to start");
    return;
  }
  // Must be set AFTER softAP() (see AP_TX_POWER in board.h).
  WiFi.setTxPower(AP_TX_POWER);
  g_apUp = true;
  Serial.printf("[WIFI] fallback SoftAP up: ssid='%s' ip=192.168.4.1\n", AP_SSID);
}

static void stopFallbackAp() {
  WiFi.softAPdisconnect(true);
  WiFi.setTxPower(g_defaultTxPower);
  g_apUp = false;
  Serial.println("[WIFI] fallback SoftAP dropped (STA connected)");
}

void wifiBegin() {
  // We own the connection lifecycle; keep the SDK from auto-restoring or
  // caching credentials behind our back.
  WiFi.persistent(false);
  g_defaultTxPower = WiFi.getTxPower();

  if (config.wifiSsid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);  // modem sleep (STA default, set explicitly)
    WiFi.begin(config.wifiSsid, config.wifiPass);
    g_downSince = millis();  // start the D2 fallback clock from boot
    Serial.printf("[WIFI] joining '%s' as client\n", config.wifiSsid.c_str());
  } else {
    WiFi.mode(WIFI_AP);
    startFallbackAp();
    Serial.println("[WIFI] no credentials set; fallback SoftAP only");
  }
}

void wifiLoop() {
  unsigned long now = millis();
  bool connected = wifiStaConnected();

  if (connected) {
    if (!g_wasConnected) {
      g_wasConnected = true;
      g_downSince = 0;
      g_retryInterval = RECONNECT_BASE_MS;
      Serial.printf("[WIFI] connected ssid='%s' ip=%s rssi=%d dBm\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                    (int)WiFi.RSSI());
      if (MDNS.begin(MDNS_NAME)) {
        Serial.printf("[WIFI] mDNS registered at http://%s.local\n", MDNS_NAME);
      }
    }
  } else {
    if (g_wasConnected) {
      g_wasConnected = false;
      g_downSince = now;
      MDNS.end();
      Serial.println("[WIFI] connection lost");
    }
  }

  // --- STA reconnect with backoff (the core also retries internally; this
  // re-primes the attempt in case its state gets stuck) ---
  if (config.wifiSsid.length() > 0 && !connected) {
    if (g_downSince == 0) g_downSince = now;  // defensive
    if (now - g_lastAttempt >= g_retryInterval) {
      g_lastAttempt = now;
      if (g_retryInterval < RECONNECT_MAX_MS) g_retryInterval *= 2;
      WiFi.begin(config.wifiSsid, config.wifiPass);
      Serial.printf("[WIFI] STA retry (next in %lus)\n",
                    g_retryInterval / 1000);
    }
  }

  // --- Fallback SoftAP (D2): up when there is nothing to connect to or the
  // STA has been down too long; it drops only once the STA connects, so a
  // client on the fallback dashboard stays through a credential save ---
  bool wantAp = (config.wifiSsid.length() == 0) ||
                (g_downSince != 0 && (now - g_downSince) >= WIFI_FALLBACK_AFTER_MS);
  if (wantAp && !g_apUp) {
    startFallbackAp();
  }
  if (connected && g_apUp) {
    stopFallbackAp();
  }
}

void wifiApplyCredentials() {
  g_downSince     = millis();  // re-arm the D2 fallback clock
  g_retryInterval = RECONNECT_BASE_MS;
  g_lastAttempt   = 0;         // force an immediate (re)begin below
  if (config.wifiSsid.length() == 0) {
    WiFi.disconnect(true);     // clear the SDK's cached credentials too
    WiFi.mode(WIFI_AP);
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);  // modem sleep (survives the mode switch)
    WiFi.disconnect(true);
    WiFi.begin(config.wifiSsid, config.wifiPass);
  }
  Serial.printf("[WIFI] applied credentials live ssid='%s'\n",
                config.wifiSsid.c_str());
}

bool wifiStaConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool wifiOnSetupAp() {
  return g_apUp;
}
