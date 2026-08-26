#pragma once

#include <Arduino.h>

// Always-on WiFi: the device joins the home network as a STA client, with the
// open setup SoftAP as a fallback (PLAN.md D2):
//   * no credentials saved            -> SoftAP from boot
//   * credentials saved, no connection
//     for WIFI_FALLBACK_AFTER_MS      -> SoftAP comes up (STA+AP)
//   * STA connects                    -> SoftAP drops
// While the fallback AP is up the dashboard (served from Phase 4 on) is
// reachable on 192.168.4.1, so the WiFi can be reconfigured without a button.

// Start WiFi: STA if credentials are saved, fallback SoftAP otherwise.
void wifiBegin();
// Call from loop(): STA reconnect with backoff, SoftAP fallback, mDNS.
void wifiLoop();

// Apply the currently saved credentials live (no reboot): drop the old
// connection and begin with the new ones.
void wifiApplyCredentials();

// STA currently associated and IP'd.
bool wifiStaConnected();
// True while the fallback SoftAP is up (dashboard served from the AP).
bool wifiOnSetupAp();
