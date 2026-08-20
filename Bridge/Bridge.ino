#include "pin_scan.h"
#include "uart_scan.h"
#include "pairing.h"
#include "wifi_scan.h"
#include "runtime.h"
#include "latency_test.h"

// ============================================================
//  MANUAL UART PIN CONFIGURATION
//  - Default value -1 = automatic UART pin scan.
//  - If both are set to valid values (0..63), the scan is bypassed
//    and these pins are used directly for the Pico handshake.
//  MANUAL UART BAUD CONFIGURATION (FALLBACK)
//  - Default value -1 = use baudrate from handshake or NVS.
//  - If set to a valid baudrate (e.g., 115200), it is used ONLY
//    when no handshake occurs (i.e., when pins and baud are already
//    saved in NVS and the Pico does not request a new discovery).
//    During an active handshake, the baudrate sent by the Pico
//    always takes precedence.
//  - Valid Bauds are: 9600,19200,38400,57600,115200,230400,
//    460800,921600,1500000,2000000,3000000,4000000.
// ============================================================
int manualUartTxPin = -1;
int manualUartRxPin = -1;
int manualUartBaud = -1;

// ============================================================
//  MANUAL MAC ADDRESS CONFIGURATION
//  - If left as {0x00,0x00,0x00,0x00,0x00,0x00} = automatic pairing.
//  - If set with a valid MAC address (e.g., the Controller's MAC),
//    automatic pairing is bypassed and the specified MAC will be used.
//  - Broadcast {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF} is accepted and it's faster.
//  - Example: uint8_t manualPeerMac[6] = {0xA1,0xB2,0xC3,0xD4,0xE5,0xF6}.
//  MANUAL CHANNEL CONFIGURATION
//  - manualChannel = -1: runs automatic scan (if the channel number is not already saved in NVS)
//  - manualChannel = 0 = uses the default discovery channel (1) everywhere
//  - manualChannel = 1 to 13 = set the manual channel (pairing stays on 1, runtime uses this)
// ============================================================
uint8_t manualPeerMac[6] = {0x00,0x00,0x00,0x00,0x00,0x00};
int8_t manualChannel = -1;

// ===============================================================================================
//  MANUAL LATENCY TEST CONFIGURATION
//  - enableLatencyTest = true: enables the background latency test with pong reply
//  - enableLatencyTest = false: completely disables the latency test on the bridge side
// ===============================================================================================
bool enableLatencyTest = false;

// ============================================================
//  MANUAL T-PICOC3 CONFIGURATION
//  - By default (0), the code assumes a generic ESP32.
//  - Set to 1 to enable T-PicoC3 mode, which restricts ADC
//    availability to GPIO2 only (the only ADC pin exposed
//    on the T-PicoC3's ESP32-C3).
//  - Note: This setting affects ADC pin mapping and channel
//    configuration for analog readings.
// ============================================================
#define TPICOC3_MODE 0   // 0 = generic ESP32, 1 = T-PicoC3

static bool runtimeStarted = false;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Bridge starting...");
    btStop();

    if (manualUartRxPin < 0 || manualUartTxPin < 0) {
        runPinScanIfNeeded();
        Serial.println("ok pin scan");
    } else {
        Serial.println("Manual UART pins: automatic pin scan bypassed.");
    }

    uartScanBegin();
    while (!uartScanReady()) {
        uartScanLoop();
        yield();
    }
    Serial.println("UART OK");
    
    pairingBegin();
    Serial.println("DEBUG: pairingBegin completed");
}

void loop() {
    pairingLoop();

    wifiScanLoop();

    if (pairingReady() && !runtimeStarted) {
        uint8_t chosenChannel = wifiChannelSelectOrApply();
        if (chosenChannel > 0) {
            Serial.println("DEBUG: calling runtimeInit");
            if (runtimeInit()) {
                runtimeStart();
                runtimeStarted = true;
            }
        }
    }

    if (pairingReady()) {
        runtimeLoop();
    }
    yield();
}