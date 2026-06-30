#include "pin_scan.h"
#include "uart_scan.h"
#include "pairing.h"
#include "wifi_scan.h"
#include "runtime.h"

// ============================================================
//  MANUAL UART PIN CONFIGURATION
//  - Default value -1 = automatic UART pin scan.
//  - If set to valid values (0..63), the scan is bypassed
//    and these pins are used directly for the Pico handshake.
// ============================================================
int manualUartTxPin = -1;
int manualUartRxPin = -1;

// ============================================================
//  MANUAL MAC ADDRESS CONFIGURATION
//  - If left as {0x00,0x00,0x00,0x00,0x00,0x00} = automatic pairing.
//  - If set with a valid MAC address (e.g., the master's MAC),
//    automatic pairing is bypassed and the specified MAC will be used.
//  - Broadcast {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF} is accepted and it's faster.
//  - Example: uint8_t manualPeerMac[6] = {0xA1,0xB2,0xC3,0xD4,0xE5,0xF6}.
//  MANUAL CHANNEL CONFIGURATION
//  - manualChannel = -1: runs automatic scan (if the channel number is not already saved in NVS)
//  - manualChannel = 0 = uses the default discovery channel (1) everywhere
//  - manualChannel = 1 to 13 = set the manual channel (pairing stays on 1, runtime uses this)
// ============================================================
uint8_t manualPeerMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
int8_t manualChannel = -1;

static bool runtimeStarted = false;

void setup() {
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