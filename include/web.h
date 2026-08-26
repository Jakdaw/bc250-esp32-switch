#pragma once

// Always-on web server: dashboard + configuration API. Runs whether the
// device is on the LAN (STA) or the fallback SoftAP (see wifi.cpp) — the
// DNSServer captive portal is only active while the fallback AP is up.
void webBegin();
void webLoop();
