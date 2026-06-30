#include "pin_scan.h"
#include "pairing.h"
#include "wifi_scan.h"
#include "runtime.h"

// ============================================================
//  MANUAL PIN CONFIGURATION
//  - Leave empty "" to let the system run the automatic scan.
//  - Fill with pin numbers to bypass the scan.
//  - Example: const char* manualDigitalPins = "13,14,15,16,17,18,20,24,34,56".
//  - Example: const char* manualAnalogPins = "0,1,2,3,4,5".
// ============================================================
const char* manualDigitalPins = "";
const char* manualAnalogPins = "";

// ==========================================================================
//  MANUAL MAC ADDRESS CONFIGURATION
//  - If left as {0x00,0x00,0x00,0x00,0x00,0x00} = automatic pairing.
//  - If set with a valid MAC address (e.g., the slave's MAC),
//    automatic pairing is bypassed and the specified MAC will be used.
//  - Broadcast {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF} is accepted and it's faster.
//  - Example: uint8_t manualPeerMac[6] = {0xA1,0xB2,0xC3,0xD4,0xE5,0xF6}.
// ==========================================================================
uint8_t manualPeerMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ===============================================================================================
//  MANUAL WIFI CHANNEL CONFIGURATION
//  - manualChannel = -1: runs automatic scan (if the channel number is not already saved in NVS)
//  - manualChannel = 0 = uses the default discovery channel (1) everywhere
//  - manualChannel = 1 to 13 = set the manual channel (pairing stays on 1, runtime uses this)
//  MANUAL ESPNOW PACING CONFIGURATION (period between espnow packets are sent)
//  - manualPacingUs = 0 = scan is performed and the calculated value is used.
//  - manualPacingUs > 0 = manual pacing in microseconds (1000 µs -> 1000 ESP-NOW packets/s)
//  WIFI SCAN TIMING CONFIGURATION
//  - testDurationMs = duration of each channel scan test in milliseconds
//  - pktIntervalUs = interval between ESP-NOW packets during the scan in microseconds
// ===============================================================================================
int8_t manualChannel = -1;
uint32_t manualPacingUs = 0;
const uint32_t testDurationMs = 1500;
const uint32_t pktIntervalUs = 500;

static bool runtimeStarted = false;

void setup() {
    Serial.begin(115200);
    runPinScanIfNeeded();
    delay(500);
    yield();

    pairingBegin();
}

void loop() {
    pairingLoop();

    if (pairingReady() && !runtimeStarted) {
        channelSelectOrApply();

        runtimeInit();
        runtimeStarted = true;
    }

    if (pairingReady()) {
        runtimeLoop();
    }
}