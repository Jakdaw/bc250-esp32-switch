# BC250 ESP32 Power Switch

An ESP32-C3 power controller for an AMD **BC250** board running as a desktop. The
BC250 is fed from a PCI-E connector and has no ATX power button, so this firmware
drives the SFX PSU's `PS_ON#` line and senses board power, giving you a real power
button.

The device is **always networked**: it joins your home WiFi as a client, serves a web
dashboard at all times (`http://bc250-switch.local`), and exposes the machine to Home
Assistant over MQTT. Power can be controlled from the button, the dashboard, or
Home Assistant; any number of Bluetooth controllers can be bound so the machine
"follows the controller" — pick one up, the machine boots.

## Features

- **Push-button power**: tap to turn on; hold 5 s while running to force off;
  hold 8 s while off to factory reset.
- **Always-on web dashboard**: live status (power state, board sense, BLE presence,
  WiFi, MQTT, uptime), power ON/OFF buttons, and full configuration — reachable on
  the LAN at `http://bc250-switch.local` (mDNS), no button holds required.
- **Follows the board**: if the OS shuts the board down, the PSU is cut automatically.
- **Boot watchdog**: if the board doesn't come up within 10 s, the PSU is released.
- **Multi-controller BLE wake**: bind any number of controllers (8BitDo etc.);
  while the machine is off, *any* of them powering on wakes it.
- **SoftAP fallback**: if the device loses its WiFi connection for ~30 s, an open
  setup network comes up so the dashboard stays reachable and can be reconfigured
  — no button, no reflash.
- **MQTT + Home Assistant**: auto-discovered switch plus board-up and sense-voltage
  sensors; commands from HA; LWT-based availability.

## Wiring

The ESP32-C3 is permanently powered from the ATX connector's **5 V standby**, so it
runs whether the machine is on or off. Share a common ground between the ESP, the PSU,
and the board.

| ESP32-C3 | Connects to | Notes |
|----------|-------------|-------|
| GPIO5 | Momentary switch, terminal A | Read with internal pull-up |
| GPIO6 | Momentary switch, terminal B | Driven LOW as the switch's ground |
| GPIO4 | ATX `PS_ON#` (green wire) | **Open-drain**, active LOW: LOW = PSU on, released = off |
| GPIO3 | BC250 `TPMS1` (pin 9) | ~3.3 V when the board is up, 0 when off UPDATE: Easier to use a pin on J4003  - see below |
| 5VSB / GND | PSU standby + common ground | Permanent power for the ESP |

`PS_ON#` idles at ~5 V (pulled up inside the PSU). GPIO4 is driven open-drain so the
3.3 V part never fights the 5 V rail — it only ever sinks to ground to switch the PSU on.

`TPMS1` is a higher-impedance signal that hovers near the logic threshold, so it's read
as an analog voltage with hysteresis rather than a digital pin.

fpasteau on BC250 Discord pointed out that it's much easier to sense board-power from
the `J4003` header, which has a more convenient pitch and is right next to the power
connectors and away from fans & 3D printed shrouds. Pin 12 on this header (though 6, 8
or 10 would also be candidates) is actually an input pin for a fan sensor but is pulled
high to 3.3v so can be used in exactly the same way as the pin on `TPMS1`.

### Connector pinouts

**ATX 24-pin main connector** — tap three pins:

```
                +3.3V ─┤  1 │ 13 ├─ +3.3V
                +3.3V ─┤  2 │ 14 ├─ −12V
                  GND ─┤  3 │ 15 ├─ GND
                  +5V ─┤  4 │ 16 ├─ PS_ON#   ◄── GPIO4  (green, open-drain, active LOW)
                  GND ─┤  5 │ 17 ├─ GND      ◄── ESP GND (any GND pin works)
                  +5V ─┤  6 │ 18 ├─ GND
                  GND ─┤  7 │ 19 ├─ GND
               PWR_OK ─┤  8 │ 20 ├─ (RSVD)
 ESP 5V/VIN ──► +5VSB ─┤  9 │ 21 ├─ +5V
                 +12V ─┤ 10 │ 22 ├─ +5V
                 +12V ─┤ 11 │ 23 ├─ +5V
                +3.3V ─┤ 12 │ 24 ├─ GND
```

**TPMS1 header** — old suggestion as a single pin for board-power sense:

```
    PCICLK ─┤  1   2 ├─ GND
     FRAME ─┤  3   4 ├─ SMB_CLK_MAIN
   PCIRST# ─┤  5   6 ├─ SMB_DATA_MAIN
      LAD3 ─┤  7   8 ├─ LAD2
        3V ─┤  9  10 ├─ LAD1      ◄── pin 9 (3V) = board-on sense ──► GPIO3
      LAD0 ─┤ 11  12 ├─ GND
           ─┤     14 ├─ S_PWRDWN#
      3VSB ─┤ 15  16 ├─ SERIRQ#
       GND ─┤ 17  18 ├─ GND
```

**J4003 header** - better single pin for board-power sense:


```
                                                      ─┤     15 ├─ GND
                                            FAN_CRTL# ─┤ 14  13 ├─ GND
      GPIO3 ──► pin 12 used for sense  ──► SYSFANIN_H ─┤ 12  11 ├─ SYSFANOUT_H
                                          AUXFANIN2_H ─┤ 10   9 ├─ AUXFANOUT2_H
                                          AUXFANIN1_H ─┤  8   7 ├─ AUXFANOUT1_H
                                          AUXFANIN0_H ─┤  6   5 ├─ AUXFANOUT0_H
                                           CPUFANIN_H ─┤  4   3 ├─ CPUFANOUT_H
                                                  GND ─┤  2   1 ├─ GND
```

J4003 Pin 12 is the only connection point on the BC250 used: it reads ~3.3 V when
the board is powered and 0 V when off. No ground wire is needed from this header
— the ESP already shares ground with the board through the ATX connector.

## Button controls

| Action | Result |
|--------|--------|
| Tap while **off** | Power on |
| Hold ≥ 5 s while **on** | Force power off |
| Hold ≥ 8 s while **off** | **Factory reset**: wipes all config (password, WiFi, MQTT, bound controllers) and reboots into the setup SoftAP |

The button is the primary control and always works, even with no controller configured.

## First-time setup

1. Flash (see [Build & flash](#build--flash)). With no WiFi credentials saved, the
   device boots straight into the fallback SoftAP.
2. Connect to the open network **`BC250 Switch Setup`** and open `http://192.168.4.1`.
3. **Create a password** (minimum 6 characters). Every later visit to the dashboard
   asks for it.
4. Save your home **WiFi SSID + password** — the device connects to the LAN
   immediately (live apply, no reboot) and the SoftAP drops. The dashboard is now at
   `http://bc250-switch.local`.
5. Optionally bind controllers and enable MQTT (below).

## Web dashboard

Served from SPIFFS at `/` (plus `/api/*`), always — on the LAN or from the fallback
SoftAP (a banner tells you when you're on the fallback). Polls `/api/status` every 2 s.

- **Status** — power state (OFF/BOOTING/ON), board up/down with board-power sense voltage,
  controller presence, WiFi (SSID/IP/RSSI), MQTT connection, uptime.
- **Power** — big ON/OFF buttons. "Off" is a hard PSU cut (same as the 5 s button
  hold); "on" while booting is a no-op; "off" while booting aborts the boot.
- **Controllers** — the bound list with per-entry remove; add via a live BLE scan
  picker (name + MAC + RSSI) or manual MAC entry. An empty list simply disables
  BLE wake.
- **WiFi** — SSID + password, applied live on save. This is how you fix a changed
  home network.
- **MQTT** — enable, host, port, username (optional), password, device name.
- **Security** — change the dashboard password.
- **Factory reset** — full wipe + reboot into the setup SoftAP (same as the 8 s hold).

### HTTP API (bearer token = your dashboard password)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/status` | GET | Live state (public — no token needed) |
| `/api/power` | POST | `{"action": "on" \| "off"}` |
| `/api/ble/add` / `/api/ble/remove` | POST | `{"addr": "aa:bb:cc:dd:ee:ff"}` |
| `/api/ble/devices` | GET | Live scan results for the picker |
| `/api/wifi` | POST | `{"ssid": ..., "pass": ...}` (live apply) |
| `/api/mqtt` | POST | `{"enabled", "host", "port", "user", "pass", "name"}` |
| `/api/password` | POST | Change the dashboard password |
| `/api/factory` | POST | Full reset + reboot |

## SoftAP fallback

If saved WiFi credentials exist but the device has been **disconnected for 30 s**, it
brings up the open SoftAP **`BC250 Switch Setup`** (192.168.4.1) alongside the STA —
so a killed router or changed password never bricks remote access. The SoftAP drops
again as soon as the STA reconnects. The same dashboard is served in both modes.

## Bluetooth controller wake

Bind one or more controllers from the dashboard. While the machine is **off**, any
bound controller being present (advertising within the last 4 s) powers it on —
"machine follows controller". After any power-off there's a 15 s guard window so the
controller's post-shutdown reconnect burst can't immediately switch the machine back
on: switch the controller off within that window to keep the machine down.

The scan is passive (low radio duty) so it coexists with WiFi on the C3's single radio.

## MQTT + Home Assistant

Enable in the dashboard's MQTT section and save. The device connects with backoff,
publishes **retained** state, registers an LWT (`offline`) on the availability topic,
and republishes everything (including discovery) on each (re)connect.

Home Assistant auto-discovers one device with three entities:

| Entity | Type | Notes |
|--------|------|-------|
| `<device name>` (default `bc250-switch`) | switch | commands `ON` / `OFF` / `TOGGLE`; BOOTING reports as ON |
| `board up` | binary_sensor | device class `power`; follows the board-power sense |
| `sense voltage` | sensor | board-power sense reading in mV, refreshed every 5 s |

Topics (uid = `bc250-switch-<mac>`, fixed for the hardware so entities survive renames):

```
bc250/<uid>/state         retained  "ON" | "OFF"
bc250/<uid>/set           command   "ON" | "OFF" | "TOGGLE"
bc250/<uid>/availability  retained  "online" | "offline"  (LWT)
bc250/<uid>/board         retained  "ON" | "OFF"
bc250/<uid>/sense         retained  mV as a number
homeassistant/switch/<uid>/config
homeassistant/binary_sensor/<uid>-board/config
homeassistant/sensor/<uid>-sense/config
```

The MQTT username is optional; the password is sent only when a username is set.
Killing the broker (or the WiFi) makes HA mark the device unavailable via the LWT;
state is republished on reconnect.

## Power

The device never deep-sleeps (it must stay reachable for BLE wake and the web
server). To keep the standby draw low it runs at **80 MHz** (logged at boot:
`[INIT] CPU clock: 80 MHz`), idles the main loop task 5 ms per pass, enables WiFi
modem sleep, and uses a low-duty passive BLE scan. Expected idle current is
~50–80 mA with WiFi STA + passive BLE; verify with a meter.

## Build & flash

PlatformIO (pioarduino). Two steps — firmware and the dashboard (a single
`app/index.html` packed into SPIFFS):

```bash
pio run -t upload     # firmware
pio run -t uploadfs   # web UI filesystem
```

## Notes

- **WiFi TX power**: these ESP32-C3 *mini* boards have an RF/power quirk
  ([arduino-esp32 #6551](https://github.com/espressif/arduino-esp32/issues/6551)) where
  the SoftAP is invisible at full power. The fallback AP runs at `WIFI_POWER_8_5dBm`
  (`AP_TX_POWER` in [include/board.h](include/board.h)); normal STA power is restored
  when the AP drops.
- Serial debug runs over USB-CDC at **115200** baud; a heartbeat line is printed every
  30 s.
- Configuration lives in NVS (20 KB) and survives reboot; a factory reset (button or
  dashboard) wipes it.
- Pin assignments and all timing constants live in [include/board.h](include/board.h).
