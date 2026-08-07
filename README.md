# Esp32-lowlatency-wireless-gp2040ce-controller

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/platform-ESP32-blue.svg)](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller)
[![Firmware](https://img.shields.io/badge/pico%20firmware-GP2040--CE--UART-informational.svg)](https://github.com/Jamaica-Sound/GP2040-CE-UART)
[![Wiki](https://img.shields.io/badge/docs-wiki-brightgreen.svg)](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki)

A low-latency, wireless input bridge for the Raspberry Pi Pico running **[GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART)**. Two ESP32 boards replace the physical wiring between your buttons/joysticks and the Pico with an **ESP-NOW** radio link — one board reads the peripherals, the other forwards the data to the Pico over UART, which then appears to the host exactly as a standard GP2040-CE controller.

> **Status:** actively developed. Digital and analog inputs work reliably; triggers and rotary encoders work but need further testing, and their calibration through the GP2040-CE-UART web configurator is not yet functional. Feedback on latency performance is especially welcome.

## Table of Contents

- [How It Works](#how-it-works)
- [Protocol Overview](#protocol-overview)
- [Key Characteristics](#key-characteristics)
- [Hardware](#hardware)
- [Software Requirements](#software-requirements)
- [Quick Start](#quick-start)
- [Configuration](#configuration)
- [Documentation / Wiki](#documentation--wiki)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [Support](#support)
- [License](#license)
- [Credits](#credits)

## How It Works

```mermaid
flowchart LR
    P["Buttons, D-pad,<br/>joysticks, triggers"] --> C["Controller (ESP32)"]
    C -- "ESP-NOW, wireless" --> B["Bridge (ESP32)"]
    B -- "UART" --> PICO["Raspberry Pi Pico<br/>(GP2040-CE-UART)"]
    PICO -- "USB" --> HOST["PC / Console"]
```

The project is made of two independent Arduino sketches:

- **`Controller/Controller.ino`** — wired to your buttons, D-pad and analog inputs. Samples them continuously and streams the state to the Bridge over ESP-NOW.
- **`Bridge/Bridge.ino`** — wired via UART to the Raspberry Pi Pico. Receives the ESP-NOW stream and forwards it, byte for byte, onto the UART line.

Both sides are designed to be **plug-and-play**: pin mapping, UART baud rate, ESP-NOW peer, Wi-Fi channel and packet pacing can each be left on automatic discovery, or hard-coded manually for a faster, deterministic boot. See [Configuration](#configuration) below.

## Protocol Overview
The communication protocol is based on a custom, low‑latency packet structure defined in `protocol_v2.h`, the Controller and Bridge exchange two types of packets over ESP‑NOW:

1. **Configuration Packets**
   - Contain the effective count and the pin number of **up to 64 digital inputs and analog axes** configured or detected.
   - Sent every second from Controller to Bridge via espnow with Sync and crc.
   - For 16 buttons → 16 digital inputs + 1 D-pad → 4 digital inputs + 2 analog joystick → 4 analog axes = 31 bytes per packet.
  
2. **Runtime Packets**  
   - Contain the values detected of **up to 64 digital inputs and analog axes**.
   - Sent periodically from Controller to Bridge via espnow at a configurable or best automatic rate with Sync and crc.
   - For 16 buttons → 16 digital inputs + 1 D-pad → 4 digital inputs + 2 analog joystick → 4 analog axes = 21 bytes per packet.

All packets are CRC‑protected. The Bridge verifies the header before forwarding data to the Pico via UART that verifies the CRC, ensuring data integrity.

The configuration packets are sent every second just to be sure that it is received by the pico in a short time.

A good runtime packet rate with a paired MAC address (not broadcast) is 800pkt/s - 1 every 1.25ms, but **the rate can be 1.5 times faster if broadcast MAC is used.**

### Available Baud Rates

| Baudrate (bps) |
|----------------------------------------------------------------------------------------------------------|
| 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1500000, 2000000, 3000000, 4000000 |

### Some Controller Examples (Maximuim packets per second for All Inputs. Full table at [Communication Protocol](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Communication-Protocol))

| # | Device / Configuration | Digital Inputs (total) | Analog Axes (total) | Packet Size (bytes) | Pkt/s at 115200 | Pkt/s at 230400 | Pkt/s at 460800 | Pkt/s at 921600 | Pkt/s at 1500000 | Pkt/s at 2000000 | Pkt/s at 3000000 | Pkt/s at 4000000 |
|---|------------------------|---------------|-------------|---------------------|--------|--------|--------|--------|------|------|------|------|
| 1 | NES Controller (D-pad + A/B + Start/Select) | 8 | 0 | 13 | 886 | 1,772 | 3,545 | 7,089 | 11,538 | 15,385 | 23,077 | 30,769 |
| 2 | PlayStation 3/4 (20 buttons + 2 sticks + 2 triggers → 6 axes) | 20 | 6 | 25 | 461 | 922 | 1,843 | 3,686 | 6,000 | 8,000 | 12,000 | 16,000 |
| 3 | Standard Fightstick (8 action buttons + D-pad 4 + Start/Select → 14 dig.) | 14 | 0 | 13 | 886 | 1,772 | 3,545 | 7,089 | 11,538 | 15,385 | 23,077 | 30,769 |
| 4 | Leverless Fightstick (Hitbox style, 20 dig., 0 axes) | 20 | 0 | 13 | 886 | 1,772 | 3,545 | 7,089 | 11,538 | 15,385 | 23,077 | 30,769 |
| 5 | Arcade Stick 1 Player (digital joystick + 12 buttons + spinner + trackball + Start/Coin + 4 buttons macros,menu,volume+- → 28 dig. total) | 28 | 0 | 13 | 886 | 1,772 | 3,545 | 7,089 | 11,538 | 15,385 | 23,077 | 30,769 |
| 6 | Arcade Cabinet 2 Players (2 digital joysticks + 6 buttons each + Start/Coin each → 24 dig. total) | 24 | 0 | 13 | 886 | 1,772 | 3,545 | 7,089 | 11,538 | 15,385 | 23,077 | 30,769 |
| 7 | Maximum Possible Configuration (64 digital inputs + 8 analog axes) | 64 | 8 | 29 | 397 | 794 | 1,589 | 3,178 | 5,172 | 6,897 | 10,345 | 13,793 |

## Key Characteristics

- **Custom low-overhead binary protocol (`JSV2`)** over ESP-NOW — a compact configuration packet (pin layout) plus a compact runtime packet (input state), both CRC16-protected, sized dynamically to the number of inputs actually in use (as few as ~20 bytes for a typical layout). Full byte-level breakdown in the [Communication Protocol](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Communication-Protocol) wiki page.
- **Latency-oriented architecture** — continuous input sampling is decoupled from radio transmission via a hardware `esp_timer` and a dedicated, highest-priority FreeRTOS task on the Controller; the Bridge hands off received data to its UART task through a direct task notification rather than polling. Detailed in [Runtime Data Pipeline](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Runtime-Data-Pipeline).
- **Fully automatic first boot** — both boards can autonomously discover their GPIO wiring (works best with digital buttons that have a pull‑up resistor), find and pair with each other, pick the cleanest Wi-Fi channel, auto-tune their packet rate, and (on the Bridge) negotiate a UART baud rate with the Pico — no configuration required beyond flashing the sketches.
- **Manual override for everything automatic** — every discovered parameter can instead be hard-coded for a near-instant, deterministic boot. See [Configuration Reference](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Configuration-Reference).
- **Persistent discovery** — once pairing, pin scanning, channel selection and UART negotiation succeed, the result is cached in flash (NVS); a normal power cycle reconnects almost instantly without repeating any scan.
- **Optional broadcast mode** — pairing to the ESP-NOW broadcast address instead of a specific peer MAC is reported to reach up to ~1.5× the packet rate of a paired unicast link, at the cost of not being addressed to a single specific peer.

## Hardware

- 2× **ESP32-S3-N16R8** (dual USB) — the tested reference hardware. Other (dual core) ESP32 variants are expected to work but are untested.
- 1× **Raspberry Pi Pico** (or similar RP2040 board) running [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART).
- Buttons, joysticks, triggers, or other peripherals wired to the Controller.

Full wiring guidance, GPIO/ADC constraints and module-specific cautions: [Hardware and Wiring](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Hardware-and-Wiring).

## Software Requirements

- Arduino IDE
- ESP32 board package by Espressif Systems, version **3.3.8**
- No third-party libraries — only the standard ESP32 Arduino core (`WiFi`, `esp_now`, `esp_wifi`, `Preferences`, `HardwareSerial`, the ESP-IDF continuous ADC driver, and FreeRTOS)

## Quick Start

```bash
git clone https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller.git
cd Esp32-lowlatency-wireless-gp2040ce-controller
```

1. Open `Controller/Controller.ino` in the Arduino IDE, leave the manual parameters at their defaults for full automatic setup, select your board/port, and upload.
2. Open `Bridge/Bridge.ino`, same defaults, select board/port, upload.
3. Power on the Bridge, then the Controller. Watch both serial monitors at **115200 baud**.
4. On first boot, expect: a one-time pin scan and self-reboot on both boards, a UART handshake between the Bridge and the Pico, ESP-NOW pairing, and (once) a ~30–60 second Wi-Fi channel scan on the Controller. Subsequent boots skip all of this and reconnect immediately.
5. Flash the Pico with [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART), open its **UART Inputs Configuration** web page, set TX/RX Pin to match your wiring, and turn on **Auto-Handshake** — the Bridge's UART handshake in step 4 has nothing to do this without it, since it's off on the Pico by default.

Full step-by-step walkthrough with expected log output at each stage: [Installation and Setup](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Installation-and-Setup).

## Configuration

Every manual/automatic switch lives as a constant at the top of the corresponding `.ino` file — there is no runtime UI.

| Board | Parameter | Default | Purpose |
|---|---|---|---|
| Controller | `manualDigitalPins`, `manualAnalogPins` | `""` (auto scan) | Fixed GPIO lists instead of automatic pin discovery |
| Controller | `manualPeerMac` | `{0x00,0x00,0x00,0x00,0x00,0x00}` (auto pairing) | Fixed Bridge MAC, or broadcast for max throughput |
| Controller | `manualChannel` | `-1` (auto scan) | Fixed Wi-Fi channel (1–13), or `0` for channel 1 always |
| Controller | `manualPacingUs` | `0` (auto-tuned) | Fixed microsecond interval between runtime packets |
| Controller | `testDurationMs` | `1500` | Duration (in milliseconds) of each wifi channel scan test |
| Controller | `pktIntervalUs` | `500` | Interval between packets during wifi channel scanning (in microseconds) |
| Bridge | `manualUartTxPin`, `manualUartRxPin` | `-1` (auto scan) | Fixed UART pins toward the Pico |
| Bridge | `manualUartBaud` | `-1` (auto negotiated) | Fallback baud rate |
| Bridge | `manualPeerMac`, `manualChannel` | same as Controller | Mirror settings for the Bridge side |

**Automatic operations on Controller:**
- **Pin Scan**: Detects which pins have buttons connected (pull‑up with a resistor) or analog devices. Works best with digital buttons that have a pull‑up resistor; analog scan works out‑of‑the‑box.
- **Pairing**: Broadcasts a pairing request on a default channel and waits for the Bridge to respond and then an handshake is done.
- **Wi‑Fi Channel Scan**: Tests each channel (1‑13) by sending test packets to the Bridge and when done receive and set the best one.
- **Pacing Auto‑tuning**: Adjusts the packet send rate based on the measured channel quality to minimize latency without overloading the link.

**Automatic operations on Bridge:**
- **UART Pin Scan**: Detects which GPIO pins are connected to the Pico's UART (with an handshake procedure). This only finds anything if the Pico is actively probing for it — see the note under Configuration below.
- **Pairing**: Listens for pairing requests from the Controller on a default channel and responds with its MAC address.
- **Wifi Channel Synchronisation**: Once the Channel Scan is done, the Bridge switches to the best channel found and share the results with the Controller.
- **Baudrate Selection**: If the baudrate is set to its default/automatic value in the Bridge.ino, it will automatically receive the value configured in the web configuration page of the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) addon, during the handshake phase.

> **Note:** the Bridge's automatic UART pin scan and handshake only has something to find if the Pico's own **Auto-Handshake** switch is turned on in its **UART Inputs Configuration** web page (it's off by default) — with it off, the Pico opens its UART directly in "trust mode" and never participates in the discovery/handshake exchange described above. If you'd rather not touch that setting, set `manualUartTxPin`/`manualUartRxPin`/`manualUartBaud` on the Bridge to fixed values matching the Pico's own configuration instead. Full details: [Bridge UART Discovery](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Bridge-UART-Discovery#the-pico-side-of-this-exchange--and-why-it-matters-here).

Complete parameter tables, valid value ranges, accepted UART baud rates, and the NVS storage layout used for caching discovery results: [Configuration Reference](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Configuration-Reference).

## Documentation / Wiki

This README covers the essentials. The project [**Wiki**](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki) is a full code-verified reference.

| Page | Content |
|---|---|
| [Architecture Overview](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Architecture-Overview) | The three physical devices, end-to-end data flow, boot sequence of each sketch |
| [Communication Protocol](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Communication-Protocol) | The `JSV2` packet format, CRC16, exact packet sizes |
| [Pairing Process](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Pairing-Process) | The ESP-NOW discovery/handshake, manual and broadcast modes |
| [Wi-Fi Channel Scan and Pacing](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/WiFi-Channel-Scan-and-Pacing) | The 13-channel quality scan and the auto-tuned packet pacing algorithm |
| [Controller Pin Scan](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Controller-Pin-Scan) | The two-phase automatic GPIO classification (digital / analog / floating / unsafe / empty) |
| [Bridge UART Discovery](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Bridge-UART-Discovery) | How the Bridge finds its UART pins and negotiates a baud rate with the Pico |
| [Runtime Data Pipeline](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Runtime-Data-Pipeline) | The real-time sampling, buffering and transmission path on both boards |
| [Configuration Reference](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Configuration-Reference) | Every manual/automatic parameter, valid values, and the NVS storage map |
| [Hardware and Wiring](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Hardware-and-Wiring) | Tested hardware, GPIO/ADC constraints, wiring guidance |
| [Installation and Setup](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Installation-and-Setup) | Full step-by-step setup with expected log output |
| [Troubleshooting and Debugging](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Troubleshooting-and-Debugging) | Reading serial logs, common stuck states, forcing a clean re-scan |
| [Technical Notes and Known Behavior](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Technical-Notes-and-Known-Behavior) | Code-verified edge cases and discrepancies between docs and implementation |
| [Roadmap, Contributing and License](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Roadmap-Contributing-and-License) | Project status, planned features, contribution guidelines |

## Roadmap

- A **T-PicoC3** (Pico + ESP32-C3 on one board) is currently on the bench for a possible single-board Bridge variant.
- Full GP2040-CE LCD support for the ESP32 Controller.
- Full GP2040-CE LED support for the ESP32 Controller.
- Expanding the number of supported inputs.
- Independent LCD support with additional features.

## Contributing

Contributions are welcome — feel free to submit a Pull Request. See [Roadmap, Contributing and License](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/wiki/Roadmap-Contributing-and-License) for suggested areas to start from.

## Support

For questions or issues, please [open an issue](https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller/issues) on the GitHub repository.

## License

Licensed under the [MIT License](LICENSE).

## Credits

- **Author:** Jamaica Sound
- Inspired by and built upon the [GP2040-CE](https://github.com/OpenStickCommunity/GP2040-CE) project
- Uses the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) firmware fork for the Raspberry Pi Pico
