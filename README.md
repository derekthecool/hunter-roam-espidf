# hunter-roam-espidf

> ESP-IDF firmware that gives any Hunter sprinkler controller with a **REM port** (X-Core, X2, Pro-C, ICC, ACC) local WiFi + MQTT control — no Hunter WiFi board, no Hydrawise cloud, no subscription.

Uses the ESP32 **RMT peripheral** to drive Hunter's proprietary one-wire Roam/SmartPort protocol with cycle-accurate µs timing and zero CPU time during the ~3-second frame. Native MQTT to Home Assistant, with HA discovery — switches appear automatically under Settings → Devices.

## Why ESP-IDF + RMT?

Every existing Arduino-based Hunter integration (`seb821`, `ecodina`, `anubisg1`, `hjzimmer`, `komal-SkyNET`) bit-bangs the protocol with `delayMicroseconds()` busy-loops. That works on a single-tasking ESP8266 but jitters on a dual-core ESP32 running WiFi + MQTT. The RMT peripheral is purpose-built for this: it transmits the entire 15-byte frame autonomously while the CPU handles network traffic.

- **Cycle-accurate**: hardware-driven, ±1 µs precision
- **Zero CPU during transmit**: 3-second frame transmits in the background
- **No jitter**: immune to WiFi/MQTT interrupt latency
- **Pure ESP-IDF**: native FreeRTOS, native `esp_wifi` + `esp_mqtt`, native OTA

## Compatible controllers

Any Hunter controller with a REM terminal:

| Controller | Status |
|---|---|
| Hunter X2 | ✅ Primary target (the dev's actual unit) |
| Hunter X-Core | ✅ Confirmed across community projects |
| Hunter Pro-C | ✅ Confirmed (`ecodina` uses one) |
| Hunter ICC | ✅ Same REM protocol |
| Hunter ACC | ✅ Same REM protocol |
| Hunter Node / SRC (no REM) | ❌ Buy a different controller |

If your Hunter has a `REM` and two `24VAC` terminals in the wiring bay, this firmware works.

## Hardware

| Component | Notes |
|---|---|
| **ESP32 dev board** | Any ESP32 variant works (RMT is universal). Tested with ESP32-DevKitC. ESP32-C3 SuperMini (~$3) is a great choice — fits inside the Hunter enclosure. |
| **USB power supply** | ⚠️ **Must be a floating 2-prong charger** (most phone chargers). NOT an earth-referenced USB hub or grounded supply — see wiring below. |
| **2 wires** | ~10 cm each. Any gauge works — signal current is negligible. |

## Wiring

```
        ┌─ Floating 5V USB ─┐
        │   charger (NOT    │
        │   earth-referenced│ ← CRITICAL: must be a 2-prong
        │   — phone charger │   phone charger, not a USB
        │   is fine)        │   hub with grounded mains
        └──────┬──────┬─────┘
           5V (red)  GND (black)
               │        │
        ┌──────┴────────┴─────┐
        │     ESP32 / D1 Mini │
        │                     │
        │  GPIO4 (default)GND │ ← GPIO configurable via menuconfig
        └─────┬──────────┬────┘
              │          │
              │          │
         ┌────┴────┐ ┌───┴─────┐
         │  REM    │ │  AC2    │ ← Hunter's 24VAC common
         │terminal │ │terminal │   (one leg of secondary)
         └─────────┘ └─────────┘
              ▲
              └── single signal wire drives the REM pin
                  (Hunter's 24VAC solenoid driver handles
                   the actual valve switching)
```

**Why the floating supply matters**: The REM port is a 5VDC logic signal referenced to the AC2 (24VAC) terminal. We tie ESP32 GND to AC2 to make the GPIO swing properly against Hunter's logic threshold. If the USB supply has an earth-ground reference (e.g., a USB hub with grounded mains), tying ESP32 GND to AC2 shorts one leg of the 24VAC transformer to earth through the building ground. A standard 2-prong phone charger is floating and safe.

**Why this works without replacing the Hunter board**: the Hunter controller already does the heavy lifting — 24VAC generation, solenoid driving, the physical UI, the stored programs. This firmware just sends commands via the REM pin, like Hunter's own Roam remote would. Everything else on the Hunter keeps working.

## Build & flash

### Prerequisites

ESP-IDF v6.x installed and sourced. See the [official install guide](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/get-started/).

### Steps

```bash
# 1. Set target (esp32, esp32c3, esp32s3 — all work)
idf.py set-target esp32

# 2. Configure — fill in WiFi + MQTT
idf.py menuconfig
#   → Hunter WiFi/MQTT
#       WiFi SSID             = "your-ssid"
#       WiFi Password         = "your-password"
#       MQTT hostname prefix  = "hunter-x2"      (default)
#       MQTT broker URI       = "mqtt://192.168.1.103:1883"   (default)
#       MQTT username         = ""               (default)
#       MQTT password         = ""               (default)
#       Number of zones       = 8                (default; range 1-48)

# 3. Build, flash, monitor
idf.py -p /dev/ttyUSB0 flash monitor
```

On boot you should see:

```
I (xxx) main: hunter-roam-espidf booting — 8 zones
I (xxx) hunter_rmt: RMT TX channel initialised on GPIO 4 @ 1000000 Hz
I (xxx) hunter_cmd: cmd queue + 8 safety timers + task up
I (xxx) wifi: WiFi STA init complete — joining "your-ssid"
I (xxx) wifi: got IP 192.168.1.42
I (xxx) mqtt: connected to broker
I (xxx) mqtt: HA discovery published for 8 zones
```

## Running the unit tests locally

The `hunter_rmt` component has host-side unit tests that verify the protocol logic (bitfield encoder, frame builders, RMT symbol generation) without any hardware. They're a standalone CMake project (no `idf.py` needed for the tests themselves) — they just compile `hunter_rmt.c` against unity sources shipped inside ESP-IDF:

```bash
cd components/hunter_rmt/test
cmake -B build
cmake --build build
./build/hunter_rmt_test
```

Expected output: `20 Tests 0 Failures 0 Ignored OK`.

CI runs the same commands automatically on every push and pull request.

## Home Assistant integration

Once the firmware boots and connects, MQTT discovery is automatic. Each zone N in [1, ZONE_COUNT] becomes a `switch.hunter_x2_zone_N` entity. No YAML needed.

**Settings → Devices & Services → MQTT** should show a new device named `hunter-x2` (or whatever `CONFIG_HUNTER_HOSTNAME` you set), with one switch entity per zone.

### Toggling a zone

Tap the switch. HA publishes `{"action":"start","time":15}` to `hunter/hunter-x2/zone/N/set` — the firmware parses this, drives the Hunter REM pin, and the Hunter display shows zone N running. Toggling off publishes `{"action":"stop"}` and the Hunter display shows zone N off.

### Sample automation — sunset watering cycle

```yaml
alias: "Hunter: evening watering cycle"
trigger:
  - platform: sun
    event: sunset
    offset: "-00:30:00"
action:
  - service: switch.turn_on
    target: { entity_id: switch.hunter_x2_zone_1 }   # front lawn — 15 min (default)
  - delay: "00:16:00"
  - service: switch.turn_on
    target: { entity_id: switch.hunter_x2_zone_2 }   # back lawn
  - delay: "00:16:00"
  - service: switch.turn_on
    target: { entity_id: switch.hunter_x2_zone_3 }   # garden beds
```

### Custom durations from HA

The `time` field in the discovery payload defaults to 15 minutes. To run a zone for a custom duration from an automation, call `mqtt.publish` directly:

```yaml
action: mqtt.publish
data:
  topic: hunter/hunter-x2/zone/1/set
  payload: '{"action":"start","time":30}'
```

### Write-only caveat

Hunter's protocol is **write-only** — there's no way to read back the actual state of the valves from the controller. HA-side switch state is therefore "optimistic": when you toggle, HA shows the new state immediately, reflecting what we *asked* the Hunter to do, not necessarily what it's doing.

Two safety nets compensate:

1. **Firmware safety timers**: each `start_zone(N, M)` arms a one-shot FreeRTOS timer for `M` minutes. When the timer fires, the firmware sends `stop_zone(N)` automatically — even if HA never sends an explicit stop (e.g., MQTT broker is down, HA is rebooting).
2. **Hunter's own limit**: the Hunter board independently limits zone run time per its own programming.

### Topic reference

| Topic | Direction | Payload | Purpose |
|---|---|---|---|
| `hunter/<host>/zone/<N>/set` | inbound | `{"action":"start","time":15}` or `{"action":"stop"}` | Run/stop zone N |
| `hunter/<host>/zone/<N>/state` | outbound | `{"action":"start"}` / `{"action":"stop"}` | State reflection after a successful command |
| `hunter/<host>/program/<N>/set` | inbound | `{"action":"start"}` | Trigger Hunter program N (1-4) |
| `hunter/<host>/cmd` | inbound | `{"action":"stop_all"}`, `{"action":"reboot"}`, or `{"action":"ota"}` | Device-wide commands |
| `homeassistant/switch/<host>_<mac>_zone_<N>/config` | outbound | JSON discovery doc | HA auto-discovery (retained) |

### Optional: OTA updates

Set `CONFIG_HUNTER_OTA_URL` to a URL serving a firmware binary, and enable `CONFIG_HUNTER_OTA_CHECK_ON_BOOT` for auto-check on every boot. Or trigger OTA via MQTT:

```
topic: hunter/hunter-x2/cmd
payload: {"action":"ota"}
```

## Inspirational references & credits

This project stands on the shoulders of people who reverse-engineered Hunter's protocol and proved it could be done on ESP-class hardware:

- **[seb821/OpenSprinkler-Firmware-Hunter](https://github.com/seb821/OpenSprinkler-Firmware-Hunter)** — the canonical C/C++ implementation of the Hunter SmartPort protocol. The bitfield encoder in `hunter_rmt.c` is a direct port of `HunterBitfield` from this repo. License: GPL v3 (inherited).
- **[Scott Shumate — Hunter Sprinkler WiFi Remote Control (Hackster.io, 2015)](https://www.hackster.io/sshumate/hunter-sprinkler-wifi-remote-control-4ea918)** — the original reverse-engineering work and hardware design.
- **Dave Fleck** — protocol notes and timing measurements.
- **[ecodina/hunter-wifi](https://github.com/ecodina/hunter-wifi)** — Arduino/ESP8266 HTTP firmware; the `HunterRoam` C++ class that wraps the protocol nicely. Licensed GPL v3.
- **[anubisg1/hunter-wifi](https://github.com/anubisg1/hunter-wifi)** — fork of ecodina with MQTT, mDNS, and OTA. The closest predecessor to this project, but on ESP8266 and with busy-loop bit-banging.
- **[hjzimmer/hunter-x-core-control](https://github.com/hjzimmer/hunter-x-core-control)** — alternative MQTT firmware with optional OLED and DHT sensor.
- **[komal-SkyNET/zigbee-irrigation-controller](https://github.com/komal-SkyNET/zigbee-irrigation-controller)** — Zigbee variant for the XIAO ESP32-C6. The wiring diagram above is adapted (with attribution) from their excellent `real_wiring.jpg`.
- **[HA Community: Irrigation Hunter X-Core remote control using REM pin](https://community.home-assistant.io/t/irrigation-hunter-x-core-remote-control-using-rem-pin/320786)** — multi-page thread where most of this was figured out collectively.
- **[ESP-IDF RMT documentation](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32/api-reference/peripherals/rmt.html)** — official docs for the peripheral that makes this firmware meaningfully better than the Arduino predecessors.

## License

GNU General Public License v3 — inherited from `seb821` and `ecodina` (the protocol reverse-engineering lineage). See [LICENSE](LICENSE).

## Roadmap

- [x] **Phase 1**: `hunter_rmt` component — RMT-based protocol driver
- [x] **Phase 2**: WiFi + MQTT + HA discovery + OTA + safety shutoff + unit tests + CI *(this release)*
- [ ] **Phase 3**: WiFi provisioning via BLE/SoftAP so credentials don't need menuconfig
- [ ] **Phase 4**: Multi-controller support (driving >1 Hunter from one ESP32)
