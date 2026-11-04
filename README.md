# FanSpitter

Control ceiling fans via RF remote learning and MQTT commands. FanSpitter is ESP32 firmware that captures RF signals from ceiling fan remotes, stores them locally, and exposes them over MQTT and a web UI for integration with smart home platforms like OpenHAB.

## Overview

FanSpitter uses a **CC1101 RF transceiver** at **303 MHz OOK** (On-Off Keying) to:
- Capture and learn RF signals from your ceiling fan remote
- Store up to **8 named signal slots** in persistent flash memory
- Replay learned signals via bit-bang modulation
- Expose signals via **MQTT topics** for OpenHAB, Home Assistant, Node-RED
- Provide a **web UI** for standalone control and learning

## Features

- **Captive-portal setup**: WiFi, MQTT broker, credentials and OTA password are entered on first boot. Nothing is hard-coded.
- **8-slot RF storage** in NVS flash, kept across reboots and firmware updates
- **2-frame confirmation** (±20%, min 50 µs) to reject RF noise while learning
- **MQTT** with optional username/password, a configurable base topic, retained status and Last-Will "offline"
- **Web UI and JSON API** at `http://fanspitter.local/`
- **OTA updates** over WiFi (password-protected)
- **Self-healing**: loop watchdog, reboots if WiFi stays down, MQTT reconnect with backoff, and it keeps serving web, MQTT and OTA even if the CC1101 is missing
- **Factory reset** from the BOOT button

## Hardware

| Component | Notes |
|-----------|-------|
| **ESP32 DevKit** | Any ESP32 with VSPI + free GPIO pins (4 MB flash) |
| **CC1101 module** | 26 MHz crystal (**not** a 27 MHz clone) |
| **3.3 V supply** | CC1101 is sensitive to supply noise |
| **Fan remote** | Any OOK remote near 303 MHz |

### Wiring (ESP32 ↔ CC1101)

| CC1101 Pin | ESP32 Pin | Function |
|-----------|-----------|----------|
| VCC       | 3.3V      | Power |
| GND       | GND       | Ground |
| MOSI      | GPIO 23   | SPI (VSPI) |
| MISO      | GPIO 19   | SPI (VSPI) |
| SCK       | GPIO 18   | SPI (VSPI) |
| CSN       | GPIO 5    | Chip select |
| GDO0      | GPIO 2    | Data I/O (RX interrupt, TX bit-bang) |

> **GPIO 2 caveat:** GPIO 2 is an ESP32 boot-strapping pin and drives the on-board LED on many DevKits. The firmware keeps GDO0 quiet and pulled low when idle, but if USB flashing ever fails with *"Wrong boot mode detected"*, move GDO0 to **GPIO 4** and add `-DPIN_GDO0=4` to `build_flags` in `platformio.ini`. All pins are overridable this way (see [`src/config.h`](src/config.h)).

## Installation

### 1. Build and flash over USB (first install)

Requires [PlatformIO](https://platformio.org/install) (CLI or the VS Code extension).

```bash
pio run -t upload
pio device monitor
```

The first flash must be over USB. It writes the partition table (two 1.9 MB OTA slots). After that you can update over WiFi.

### 2. First boot: setup portal

With no saved WiFi, the device opens an access point called **`FanSpitter-Setup`**:

1. Join it from a phone or laptop. The captive portal opens.
2. Choose your WiFi network and enter its password.
3. Fill in the optional fields:

| Field | Meaning |
|-------|---------|
| MQTT broker host/IP | Leave blank for web-only mode |
| MQTT port | Default `1883` |
| MQTT username / password | Leave blank for anonymous brokers |
| MQTT base topic | Default `fanspitter`. Use a different one per device |
| OTA update password | Required to enable WiFi updates. Leave blank to disable OTA |

Password fields are never pre-filled (the setup AP is open). Leaving one blank keeps the stored value.

4. The device joins your network and serves `http://fanspitter.local/` (or check the serial log for its IP).

If saved WiFi is unreachable at boot (router rebooting, say), the portal opens for 3 minutes, then the device reboots and retries. If WiFi drops for 10 minutes while running, it reboots.

### 3. Changing settings / factory reset

Hold the **BOOT** button for 5–30 seconds, then release. This wipes WiFi and MQTT/OTA settings (**learned signals are kept**) and reboots into the setup portal. Holds longer than 30 s are ignored, so a USB-serial adapter holding GPIO 0 low can't wipe the device.

### 4. Updating over WiFi (OTA)

Requires an OTA password set in the portal.

```bash
FANSPITTER_OTA_PASSWORD='your-ota-password' pio run -e ota -t upload
```

The `ota` environment targets `fanspitter.local`. Override with `--upload-port <ip>` if mDNS doesn't resolve on your network.

## Usage

### Learn a signal

Slot names are 1–32 characters of `A–Z a–z 0–9 _ -` (e.g. `fan_high`, `light-toggle`). Re-using a name overwrites that slot.

**Web UI:** enter a name, click **Start Learn**, then press the remote button **twice**. The first press captures, the second confirms. You have 30 seconds.

**MQTT:**
```bash
mosquitto_pub -h <broker> -t fanspitter/learn -m "fan_high"
# press the remote button twice, then:
mosquitto_sub -h <broker> -t fanspitter/learned
# {"slot":"fan_high","ok":true,"pulses":84}
```

### Trigger a learned signal

**Web UI:** click **Send** next to the slot.

**MQTT:**
```bash
mosquitto_pub -h <broker> -t fanspitter/cmd -m "fan_high"
```

Each send replays the frame 3 times, 20 ms apart.

## MQTT topics

`fanspitter` below is the configurable base topic.

| Topic | Dir | Payload |
|-------|-----|---------|
| `fanspitter/learn` | sub | slot name to learn |
| `fanspitter/cmd` | sub | slot name to send |
| `fanspitter/status` | pub, retained | status JSON (below); `{"state":"offline"}` via Last Will |
| `fanspitter/learned` | pub | `{"slot":"fan_high","ok":true,"pulses":84}` or `{"slot":"fan_high","ok":false,"error":"timeout"}` |

Status is published on every state change and every 60 s.

### Status JSON

Served at `GET /api/status` and published to `<base>/status`:

```json
{
  "state": "idle",
  "learnSecs": 0,
  "slots": ["fan_high", "fan_med", "light_toggle"],
  "slotCount": 3,
  "maxSlots": 8,
  "ip": "192.168.1.42",
  "rssi": -58,
  "mqtt": true,
  "rf": true,
  "ota": true,
  "uptime": 12345,
  "heap": 187432,
  "version": "v1.0.0"
}
```

`state` is `idle`, `learning`, `transmitting` (or `offline` from the Last Will). `rf: false` means the CC1101 didn't respond at boot. `version` comes from `git describe` at build time.

## OpenHAB integration

```
Thing mqtt:topic:fanspitter "FanSpitter" (mqtt:broker:localBroker) {
    Channels:
        Type string : cmd     [ commandTopic="fanspitter/cmd" ]
        Type string : learn   [ commandTopic="fanspitter/learn" ]
        Type string : state   [ stateTopic="fanspitter/status", transformationPattern="JSONPATH:$.state" ]
        Type string : learned [ stateTopic="fanspitter/learned" ]
}
```

```
String FanCmd   "Fan"              { channel="mqtt:topic:fanspitter:cmd" }
String FanState "FanSpitter [%s]"  { channel="mqtt:topic:fanspitter:state" }
```

Sitemap buttons, one per learned slot:

```
Switch item=FanCmd mappings=[fan_high="High", fan_med="Med", fan_off="Off", light_toggle="Light"]
```

## HTTP API

| Method | Path | Body | Purpose |
|--------|------|------|---------|
| `GET` | `/` | | Web UI |
| `GET` | `/api/status` | | Status JSON |
| `GET` | `/api/slots` | | `["fan_high", …]` |
| `POST` | `/api/learn` | `{"slot":"name"}` | Start learning |
| `POST` | `/api/send` | `{"slot":"name"}` | Replay signal |
| `DELETE` | `/api/slot` | `{"slot":"name"}` | Delete slot |

Errors return `{"error":"…"}` with `400` (bad name/body), `404` (no such slot), `409` (busy learning), `503` (RF unavailable or queue full), or `507` (all 8 slots used).

The web UI and API have **no authentication**. Keep the device on a trusted LAN or VLAN.

## Technical details

### Architecture

| File | Role |
|------|------|
| `main.cpp` | Orchestration: setup portal, command queue, status, OTA, watchdogs |
| `cc1101.h/.cpp` | SPI driver and register table (26 MHz XO, 303 MHz, async OOK) |
| `rf_learn.h/.cpp` | IRAM ISR pulse capture, 2-frame confirmation, bit-bang TX |
| `pulse_match.h` | Pure frame-comparison logic (unit-tested) |
| `slot_name.h` | Slot-name validation (unit-tested) |
| `signal_store.h/.cpp` | Thread-safe NVS storage (8 slots × 400 pulses) |
| `mqtt_handler.h/.cpp` | PubSubClient with auth, LWT and reconnect backoff |
| `web_ui.h/.cpp` | ESPAsyncWebServer UI + JSON API |
| `config.h` | Pins and tunables, all overridable with `-D` flags |

### Threading

The web server runs in the AsyncTCP task; everything else runs in the Arduino loop task. Web and MQTT requests are validated, then **queued** to the loop task. Only the loop task touches the radio, so a web request can't bit-bang RF with interrupts masked or race the learn state machine. `SignalStore` is mutex-protected and shared state is atomic.

### Radio notes

- **Crystal:** check that your CC1101 has a **26 MHz** crystal. A 27 MHz clone shifts the carrier ~12 MHz (to ~315 MHz).
- **OOK power table:** `FREND0.PA_POWER = 1`, so `PATABLE[0] = 0x00` (carrier off for a `0`) and `PATABLE[1] = 0xC0` (≈ +10 dBm for a `1`).
- **Idle:** the CC1101 sits in IDLE and the GDO0 interrupt is detached except while learning (RX) or sending (TX).
- **TX timing:** interrupts on the loop core are masked per frame for clean timing. Frames over 200 ms are sent unmasked so the interrupt watchdog can't fire.
- **Capture polarity:** the first stored pulse is assumed to be carrier-on (line idles low between frames), and TX starts HIGH.
- **Storage:** max 400 pulses per slot.

## Development

```bash
pio test -e native        # host unit tests (no hardware)
pio run                   # firmware build
pio device monitor        # serial log, with exception decoding
```

Platform and library versions are pinned in `platformio.ini`. Bump them deliberately and re-test on hardware.

CI (`.gitea/workflows/build.yml`) runs the tests and build on every push and PR and uploads `firmware.bin`, `bootloader.bin` and `partitions.bin` as artifacts. It needs Gitea Actions enabled and a runner.

## Troubleshooting

**`[CC1101] ERROR: unexpected chip ID`**: check SPI wiring (MOSI/MISO/SCK/CSN) and the 3.3 V supply. The device keeps running with `"rf": false` so you can still reach it.

**No `FanSpitter-Setup` AP**: check power under load. The AP can take 10–30 s to appear. If WiFi was configured but is unreachable, the portal only opens for 3 minutes per boot.

**Signals not captured**: confirm GDO0 wiring, that the remote is OOK near 303 MHz, and hold it close to the antenna. Watch the serial log for `Frame 1 captured` / `Pulse N mismatch` lines.

**Sends don't work but learning does**: confirm the crystal (above), try the remote's exact frequency, and check that `Transmitted N pulses x 3` appears in the log.

**MQTT won't connect**: the log shows `[MQTT] Failed (rc=…)`. `rc=5` means bad credentials and `rc=-2` means the broker is unreachable. Retries back off up to 60 s.

**USB flashing fails ("Wrong boot mode")**: see the GPIO 2 caveat above, or update over WiFi instead.

## License

[MIT](LICENSE)

## References

- [CC1101 Datasheet](https://www.ti.com/product/CC1101)
- [PlatformIO Docs](https://docs.platformio.org/)
- [OpenHAB MQTT Binding](https://www.openhab.org/addons/bindings/mqtt/)
