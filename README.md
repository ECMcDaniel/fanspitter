# FanSpitter

Control ceiling fans via RF remote learning and MQTT commands. FanSpitter is a complete ESP32 firmware solution that captures RF signals from ceiling fan remotes, stores them locally, and exposes them over MQTT and a web UI for integration with smart home platforms like OpenHAB.

## Overview

FanSpitter uses a **CC1101 RF transceiver** at **303 MHz OOK** (On-Off Keying) to:
- Capture and learn RF signals from your ceiling fan remote
- Store up to **8 named signal slots** in persistent flash memory
- Replay learned signals via bit-bang modulation
- Expose signals via **MQTT topics** for OpenHAB or other home automation systems
- Provide a **web UI** for standalone control and learning

Perfect for integrating old (non-smart) ceiling fans into modern home automation without replacing the remote or hardware.

## Features

✅ **Zero-hardcode WiFi** — Captive portal (WiFiManager) for first-time setup  
✅ **8-slot RF storage** — NVS flash memory for learned signals  
✅ **2-frame confirmation** — Tolerance-based matching (±20%) to filter RF noise  
✅ **MQTT integration** — Ready for OpenHAB, Home Assistant, Node-RED  
✅ **Single-page web UI** — Learn, store, and trigger signals from your browser  
✅ **Live status reporting** — JSON payload with device state, IP, signal strength  
✅ **Last-Will-and-Testament** — Automatic offline status on disconnect  

## Hardware Requirements

| Component | Notes |
|-----------|-------|
| **ESP32 DevKit** | Any ESP32 with VSPI + free GPIO pins |
| **CC1101 RF Module** | 26 MHz crystal (**not** 27 MHz clone) at 303 MHz |
| **3.3V Stabilized Power** | CC1101 is sensitive to supply noise |
| **Ceiling fan remote** | Any RF remote that uses OOK modulation |

### Wiring (ESP32 ↔ CC1101)

| CC1101 Pin | ESP32 Pin | Function |
|-----------|-----------|----------|
| VCC       | 3.3V      | Power |
| GND       | GND       | Ground |
| MOSI      | GPIO 23   | SPI Data (VSPI) |
| MISO      | GPIO 19   | SPI Data (VSPI) |
| SCK       | GPIO 18   | SPI Clock (VSPI) |
| CSN       | GPIO 5    | Chip Select |
| GDO0      | GPIO 2    | Data I/O (ISR RX, bit-bang TX) |

## Installation

### 1. Prerequisites
- [PlatformIO CLI](https://platformio.org/install) or VSCode with PlatformIO extension
- CC1101 module wired per the diagram above
- WiFi network and MQTT broker (optional, but recommended)

### 2. Clone and Build
```bash
git clone https://github.com/yourusername/fanspitter.git
cd fanspitter
pio run -t upload
```

### 3. First Boot — WiFi Configuration
On first power, the device broadcasts AP `FanSpitter-Setup`:
1. Connect phone/laptop to that AP
2. Captive portal opens automatically
3. Enter your WiFi SSID, password, and **MQTT broker IP** (or skip for web-only mode)
4. Device reboots and connects to your network

Monitor serial output at **115200 baud** to see the assigned IP address.

## Usage

### Learn a Signal

#### Via Web UI
1. Open `http://fanspitter.local` (or the IP from serial output)
2. Enter a slot name (e.g., `fan_high`, `light_toggle`)
3. Click **"Start Learn"** — device enters RX mode for 30 seconds
4. Press the remote button → "Frame 1 captured"
5. Press again → "Confirmed and saved" (learns on 2nd identical press)

#### Via MQTT
```bash
mosquitto_pub -h <broker_ip> -t fanspitter/learn -m "fan_high"
# Press remote button twice
# Subscribe to see confirmation:
mosquitto_sub -h <broker_ip> -t fanspitter/learned
# Output: {"slot":"fan_high","pulses":84}
```

### Trigger a Learned Signal

#### Via Web UI
- Click **"Send"** next to any saved slot

#### Via MQTT
```bash
mosquitto_pub -h <broker_ip> -t fanspitter/cmd -m "fan_high"
```

#### Via OpenHAB
See [OpenHAB Integration](#openhab-integration) below.

## MQTT Topics

All payloads are JSON unless noted.

| Topic | Direction | Payload | Example |
|-------|-----------|---------|---------|
| `fanspitter/learn` | **Sub** | Slot name (string) | `fan_high` |
| `fanspitter/cmd` | **Sub** | Slot name to execute | `fan_high` |
| `fanspitter/status` | **Pub** | Device state + slots | `{"state":"idle","slots":["fan_high","light"],"ip":"192.168.1.100","rssi":-62}` |
| `fanspitter/learned` | **Pub** | Confirmation after learn | `{"slot":"fan_high","pulses":84}` |

## OpenHAB Integration

### OH3/OH4 Thing Configuration

Create a new MQTT Thing:

```
Thing mqtt:topic:fanspitter "FanSpitter" (mqtt:broker:localBroker) {
    Channels:
        Type string : cmd     [ commandTopic="fanspitter/cmd" ]
        Type string : learn   [ commandTopic="fanspitter/learn" ]
        Type string : status  [ stateTopic="fanspitter/status",
                                transformationPattern="JSONPATH:$.state" ]
        Type string : learned [ stateTopic="fanspitter/learned" ]
}
```

### Create Items

```
String FanHigh "Fan High" (gFan) { channel="mqtt:topic:fanspitter:cmd" }
String FanMed  "Fan Medium" (gFan) { channel="mqtt:topic:fanspitter:cmd" }
String FanOff  "Fan Off" (gFan) { channel="mqtt:topic:fanspitter:cmd" }
```

### Send Command

```
rule "Fan Control"
when
    Item FanHigh received command ON
then
    FanHigh.sendCommand("fan_high")
end
```

## API Reference

### Endpoints (Web UI)

| Method | Path | Purpose |
|--------|------|---------|
| `GET` | `/` | Serve single-page UI (HTML/JS/CSS) |
| `GET` | `/api/slots` | List all saved slots |
| `GET` | `/api/status` | Device state (JSON) |
| `POST` | `/api/learn` | `{"slot":"name"}` — start learning |
| `POST` | `/api/send` | `{"slot":"name"}` — replay signal |
| `DELETE` | `/api/slot` | `{"slot":"name"}` — delete slot |

### Status JSON

```json
{
  "state": "idle",
  "slots": ["fan_high", "fan_med", "light_toggle"],
  "slotCount": 3,
  "maxSlots": 8,
  "ip": "192.168.1.42",
  "rssi": -58,
  "uptime": 12345
}
```

## Technical Details

### Architecture

- **CC1101 driver** (`cc1101.h/cpp`) — SPI register control, MISO chip-ready polling
- **RF learner** (`rf_learn.h/cpp`) — IRAM ISR pulse capture, 2-frame confirmation, bit-bang TX
- **Signal storage** (`signal_store.h/cpp`) — NVS Preferences wrapper (8 slots, max 400 pulses/slot)
- **MQTT handler** (`mqtt_handler.h/cpp`) — PubSubClient with auto-reconnect
- **Web UI** (`web_ui.h/cpp`) — ESPAsyncWebServer + PROGMEM HTML/JS

### Key Implementation Notes

1. **MISO chip-ready polling** — Always poll MISO low after CS assert before first SPI byte (prevents register corruption)
2. **Crystal frequency** — Verify your CC1101 has a **26 MHz crystal**; 27 MHz clones shift the carrier ~3.5 MHz off-target
3. **Confirmation algorithm** — Captures 2 frames and compares pulse-by-pulse with ±20% tolerance to filter RF noise
4. **OOK polarity** — First TX pulse must match the captured polarity; system auto-detects this on boot
5. **WiFi jitter** — 20% confirmation tolerance absorbs ESP32 WiFi DMA stalls (up to ~300 µs). For critical TX, disable WiFi PS: `esp_wifi_set_ps(WIFI_PS_NONE)`
6. **NVS limits** — Maximum 400 pulses per slot (800 bytes) to stay within NVS namespace constraints

## Troubleshooting

### CC1101 not detected
```
Error: CC1101 not found (part=0xFF)
```
- Check SPI wiring (MOSI, MISO, SCK, CSN)
- Verify 3.3V supply (use oscilloscope to check for noise)
- Try a different CC1101 module (some clones have poor quality)

### WiFi AP doesn't appear
- Check ESP32 power supply under load
- Look for `FanSpitter-Setup` broadcast in WiFi scan (might appear after 10–30 seconds)

### Remote signals not captured
- Verify GDO0 is connected to GPIO 2
- Check that your remote uses OOK modulation (this won't work with ASK or FSK)
- Bring remote closer to CC1101 antenna during learning
- Check the serial monitor for ISR frame captures

### MQTT connection fails
- Verify broker IP/port entered in captive portal
- Check firewall (default MQTT port 1883)
- Look for error in serial output: `MQTT: connect failed (rc=...)`

## Building from Source

### Dependencies (auto-installed by PlatformIO)

```ini
[platformio.ini]
lib_deps =
    tzapu/WiFiManager @ ^2.0.17
    knolleary/PubSubClient @ ^2.8
    ESP32Async/ESPAsyncWebServer @ ^3.0.0
    ESP32Async/AsyncTCP @ ^3.0.0
    bblanchon/ArduinoJson @ ^7.0.0
```

### Compile and Upload
```bash
pio run -t upload
```

### Monitor Serial Output
```bash
pio device monitor -b 115200
```

## File Structure

```
fanspitter/
├── platformio.ini                 — PlatformIO config
├── src/
│   ├── main.cpp                   — Orchestration + WiFiManager
│   ├── cc1101.h / cc1101.cpp      — SPI driver + register table
│   ├── rf_learn.h / rf_learn.cpp  — ISR pulse capture + TX replay
│   ├── signal_store.h / .cpp      — NVS Preferences wrapper
│   ├── mqtt_handler.h / .cpp      — PubSubClient integration
│   └── web_ui.h / web_ui.cpp      — ESPAsyncWebServer UI
└── README.md
```

## License

[See LICENSE file](LICENSE)

## Contributing

Contributions welcome! Please fork and submit PRs. Known areas for enhancement:
- Multi-frequency support
- Signal filtering algorithms
- Bluetooth/ZigBee gateway modes

## References

- [CC1101 Datasheet](https://www.ti.com/product/CC1101)
- [ESP32 GPIO Reference](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/hw-reference/esp32_datasheet.pdf)
- [PlatformIO Docs](https://docs.platformio.org/)
- [OpenHAB MQTT Binding](https://www.openhab.org/addons/bindings/mqtt/)

---

**Happy automating!** 🏠💡
