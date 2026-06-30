# Esp32-lowlatency-wireless-gp2040ce-controller
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Low-latency ESP32 custom wireless controller using ESP‑NOW with Master/Slave scripts for UART output to the forked [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) firmware.

First of all please forgive my complete lack of experience in coding, I am totally a newbie. The code is not optimized both in terms of formatting and logic and of course is still under development. Any suggestions or feedback would be highly appreciated especially regarding the latency performance.

That said this project enables espnow wireless communication between a **Master** ESP32 (connected to joysticks, buttons, and analog peripherals) and a **Slave** ESP32, which forwards the input data via UART to a **Raspberry Pi Pico** running the **GP2040-CE-UART** firmware.

Digital and analog inputs work well. The triggers and rotary encoders work but I need to do further testing (calibrations on GP2040-CE webconfig isn't working).

## How It Works

The system is composed of two ESP32 units:

- **Master**: Reads and sends digital and analog inputs from connected peripherals via ESP‑NOW to the Slave.
- **Slave**: Receives the data via ESP‑NOW and forwards it over a UART connection to a Raspberry Pi Pico (or any other device) running the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) firmware.

## Protocol Overview
The communication protocol is based on a custom, low‑latency packet structure defined in `protocol_v2.h`, the Master and Slave exchange two types of packets over ESP‑NOW:

1. **Configuration Packets**
   - Contain the effective count and the pin number of up to 64 digital inputs and analog axes configured or detected.
   - Sent every second from Master to Slave via espnow with Sync and crc.
   - For 16 buttons → 16 digital inputs + 1 D-pad → 4 digital inputs + 2 analog joystick → 4 analog axes = 31 bytes per packet.
  
2. **Runtime Packets**  
   - Contain the values detected of up to 64 digital inputs and analog axes.
   - Sent periodically from Master to Slave via espnow at a configurable or best automatic rate with Sync and crc.
   - For 16 buttons → 16 digital inputs + 1 D-pad → 4 digital inputs + 2 analog joystick → 4 analog axes = 21 bytes per packet.

All packets are CRC‑protected. The Slave verifies the CRC before forwarding data to the Pico via UART, ensuring data integrity.

The configuration packet are sent every second just to be sure that it is received by the pico in a short time.

The runtime packet good rate with a paired MAC address (not broadcast) is 800pkt/s - 1 every 1,25ms.

**The rate can be 1.5 times faster if broadcast MAC is used.**

## Configuration Parameters

Both Master and Slave support **manual** and **automatic** configurations. If manual parameters are left at their default values (or set to specific "auto" values), the system performs an automatic scan/negotiation.

### Master Configuration (`Master/Master.ino`)

| Parameter | Type | Default (Auto) | Description |
|-----------|------|----------------|-------------|
| `manualDigitalPins` | `String` | `""` | Comma‑separated list of GPIO pins used as digital inputs (buttons, rotary). If empty, the Master performs an automatic pin scan during startup. Does not work with digital buttons without a resistance applied. |
| `manualAnalogPins` | `String` | `""` | Comma‑separated list of GPIO pins used as analog inputs (joysticks, potentiometers, triggers). If empty, automatic scan. |
| `manualPeerMac` | `uint8_t[6]` | `{0x00,0x00,0x00,0x00,0x00,0x00}` | MAC address of the Slave to pair with. If all zeros, automatic pairing is performed. |
| `manualChannel` | `int8_t` | `-1` | Wi‑Fi channel (1‑13). If `-1`, the Master scans all channels to find the best one (lowest interference). If 0 use the default channel (1) for every operation. |
| `manualPacingUs` | `uint32_t` | `0` | Interval between ESP‑NOW packets sent to slave in microseconds (e.g., `1250` = 800 packets/sec). If `0`, the rate is automatically calculated by the wifi scan, based on channel quality. |
| `testDurationMs` | `uint32_t` | `1500` | Duration (in milliseconds) of each channel scan test. |
| `pktIntervalUs` | `uint32_t` | `500` | Interval between packets during channel scanning (in microseconds). |

**Automatic operations on Master:**
- **Pin Scan**: Detects which pins have buttons connected (pull‑up with a resistor) or analog devices. Works best with digital buttons that have a pull‑up resistor; analog scan works out‑of‑the‑box.
- **Pairing**: Broadcasts a pairing request and waits for the Slave to respond and then an handshake is done.
- **Wi‑Fi Channel Scan**: Tests each channel (1‑13) by sending test packets to measure the reception rate on the slave, then selects the best one.
- **Pacing Auto‑tuning**: Adjusts the packet send rate based on the measured channel quality to minimize latency without overloading the link.

### Slave Configuration (`Slave/Slave.ino`)

| Parameter | Type | Default (Auto) | Description |
|-----------|------|----------------|-------------|
| `manualUartTxPin` | `int` | `-1` | GPIO pin used for UART TX (to Pico). If `-1`, automatic pin scan is performed. |
| `manualUartRxPin` | `int` | `-1` | GPIO pin used for UART RX (from Pico). If `-1`, automatic pin scan is performed. |
| `manualPeerMac` | `uint8_t[6]` | `{0x00,0x00,0x00,0x00,0x00,0x00}` | MAC address of the Master to pair with. If all zeros, automatic pairing is performed. |
| `manualChannel` | `int8_t` | `-1` | Wi‑Fi channel (1‑13). If `-1`, the Slave follows the channel chosen by the Master during pairing. |

**Automatic operations on Slave:**
- **UART Pin Scan**: Detects which GPIO pins are connected to the Pico's UART (by trying to send a handshake and waiting for a response).
- **Pairing**: Listens for pairing requests from the Master and responds with its MAC address.
- **Channel Synchronisation**: Once paired on the default channel, the Slave switches to the channel accordingly to the configuration.
- **baudrate selection**: The baudrate is configured in the web configuration page of the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) addon and then is exchanged with the slave.

## Hardware Tested

- **2x ESP32‑S3-N16R8** with 44 pins and dual USB. The code should work on other ESP32 variants (e.g., ESP32‑WROOM, ESP32‑C3), but this is untested.
- **1x Raspberry Pi Pico** (or similar) running the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) firmware.
- Buttons, joysticks, and analog peripherals to connect to the Master.
- USB cables for power and programming.

## Hardware on the bench
- T-PicoC3 which combine a pico and an Esp32-C3 is on the way to its tests

## Software Requirements

- **Arduino IDE** (used for development and upload).
- **ESP32 Board Package** by Espressif Systems, version **3.3.8**.
- No external libraries are required; the code uses standard ESP32 core libraries (`WiFi`, `esp_now`, `HardwareSerial`, etc.). The `esp32_adc` library might be needed for continuous ADC reading, but it is typically included in the ESP32 core.

## Installation & Setup

### 1. Clone the Repository
- git clone https://github.com/Jamaica-Sound/Esp32-lowlatency-wireless-gp2040ce-controller.git
- cd Esp32-lowlatency-wireless-gp2040ce-controller

### 2. Configure the Master
- Open the Master/Master.ino file in the Arduino IDE and adjust any manual parameters as described in the Configuration Parameters section above.
- Leave them at their default values to enable automatic configuration.

### 3. Configure the Slave
- Open the Slave/Slave.ino file in the Arduino IDE and adjust any manual parameters as described in the Configuration Parameters section above.
- Leave them at their default values to enable automatic configuration.

### 4. Upload the Code
- Select the correct ESP32 board from the Tools > Board menu.
- Select the correct port from the Tools > Port menu.
- Click the Upload button.
- Repeat this process for both the Master and the Slave.

### 5. Configure the Pico
Follow the instructions in the [GP2040-CE-UART](https://github.com/Jamaica-Sound/GP2040-CE-UART) page.

### Usage:
- Power on both ESP32 units.
- The Master will scan for the best Wi‑Fi channel, pair with the Slave, and begin sending input data.
- The Slave will receive the data via espnow and forward it via UART to the connected Pico.
- Monitor the Serial Monitor (115200 baud) on both devices for debug messages and status updates.

### Debugging:
The code includes many serial debug messages. To view them: 
- Open the Serial Monitor in the Arduino IDE (Tools > Serial Monitor).
- Set the baud rate to 115200.
- Observe the output from both the Master and the Slave.

### Future Features:
- Full GP2040-CE LCD support for the ESP32 master.
- Full GP2040-CE LED support for the ESP32 master.
- Expand the amounts of inputs.
- Indipendent LCD support with features.

## License:
This project is licensed under the MIT License – see the LICENSE file for details. 

## Credits:
Author: Jamaica Sound

Inspired by and built upon the GP2040-CE project.

Uses the GP2040-CE-UART firmware for the Raspberry Pi Pico.

## Contributing:
Contributions are welcome! Please feel free to submit a Pull Request.

## Support:
For questions or issues, please open an issue on the GitHub repository.
