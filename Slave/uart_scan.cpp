#include <Arduino.h>
#include <Preferences.h>
#include "driver/gpio.h"
#include "uart_scan.h"

enum PinState {
  PIN_UNKNOWN,
  PIN_DIGITAL,
  PIN_ANALOG,
  PIN_FLOATING,
  PIN_UNSAFE,
  PIN_NON_EXISTENT,
  PIN_EMPTY
};

static const char* NVS_NAMESPACE = "pin_scanner";
static const char* KEY_SCANNED = "scannedMask";
static const char* KEY_STATE = "st_";

const uint32_t FIXED_BAUD = 9600;

int rxEsp = -1;
int txEsp = -1;
static uint32_t uartBaud = FIXED_BAUD;

bool handshakeDone = false;
bool okBurstDone = false;

unsigned long okStartTime = 0;

int digitalPins[64];
int numDigital = 0;

int getDigitalPins() {
  Preferences prefs;
  prefs.begin(NVS_NAMESPACE, true);
  uint64_t scannedMask = prefs.getULong64(KEY_SCANNED, 0);
  prefs.end();

  int count = 0;
  for (int pin = 0; pin < 64 && count < 64; pin++) {
    if (scannedMask & (1ULL << pin)) {
      Preferences prefs2;
      prefs2.begin(NVS_NAMESPACE, true);
      char key[20];
      snprintf(key, sizeof(key), "%s%d", KEY_STATE, pin);
      uint8_t state = prefs2.getUChar(key, PIN_UNKNOWN);
      prefs2.end();
      if (state == PIN_DIGITAL) {
        digitalPins[count++] = pin;
      }
    }
  }
  return count;
}

void sendPattern(int pin, uint8_t b1, uint8_t b2, uint32_t baud) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delayMicroseconds(100);

  uint32_t bitTimeUs = 1000000 / baud;
  noInterrupts();

  digitalWrite(pin, LOW);
  delayMicroseconds(bitTimeUs);
  for (int i = 0; i < 8; i++) {
    digitalWrite(pin, (b1 >> i) & 1);
    delayMicroseconds(bitTimeUs);
  }
  digitalWrite(pin, HIGH);
  delayMicroseconds(bitTimeUs);

  digitalWrite(pin, LOW);
  delayMicroseconds(bitTimeUs);
  for (int i = 0; i < 8; i++) {
    digitalWrite(pin, (b2 >> i) & 1);
    delayMicroseconds(bitTimeUs);
  }
  digitalWrite(pin, HIGH);
  delayMicroseconds(bitTimeUs);

  interrupts();
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(10);
}

bool receivePattern(int pin, uint8_t expected1, uint8_t expected2, uint32_t baud, int timeoutMs) {
  uint32_t bitTimeUs = 1000000 / baud;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (gpio_get_level((gpio_num_t)pin) == LOW) {
      Serial.printf("[PIN %d] START BIT DETECTED\n", pin);
      delayMicroseconds(bitTimeUs + (bitTimeUs / 2));

      uint8_t data1 = 0;
      for (int i = 0; i < 8; i++) {
        int bit = gpio_get_level((gpio_num_t)pin);
        data1 |= (bit << i);
        delayMicroseconds(bitTimeUs);
      }
      Serial.printf("[PIN %d] BYTE1 = 0x%02X\n", pin, data1);
      delayMicroseconds(bitTimeUs);

      bool secondStart = false;
      unsigned long waitSecondStart = micros();
      while (micros() - waitSecondStart < (bitTimeUs * 3)) {
        if (gpio_get_level((gpio_num_t)pin) == LOW) {
          secondStart = true;
          break;
        }
      }
      if (!secondStart) {
        Serial.printf("[PIN %d] SECOND START BIT NOT FOUND\n", pin);
        continue;
      }

      uint8_t data2 = 0;
      for (int i = 0; i < 8; i++) {
        int bit = gpio_get_level((gpio_num_t)pin);
        data2 |= (bit << i);
        delayMicroseconds(bitTimeUs);
      }
      Serial.printf("[PIN %d] BYTE2 = 0x%02X\n", pin, data2);
      delayMicroseconds(bitTimeUs);

      if (data1 == expected1 && data2 == expected2) {
        Serial.printf("[PIN %d] MATCH OK -> 0x%02X 0x%02X\n", pin, data1, data2);
        return true;
      }
      Serial.printf("[PIN %d] WRONG PATTERN expected 0x%02X 0x%02X\n", pin, expected1, expected2);
    }
    delayMicroseconds(20);
  }
  return false;
}

void performHandshake() {
    Serial.println();
    Serial.println("=== FINAL UART HANDSHAKE ===");
    Serial.printf("Starting Serial2 RX=%d TX=%d\n", rxEsp, txEsp);

    Serial2.begin(FIXED_BAUD, SERIAL_8N1, rxEsp, txEsp);
    delay(100);
    Serial2.setTimeout(50);

    String msg = "";
    unsigned long startWait = millis();
    unsigned long lastOk = 0;

    while (millis() - startWait < 3000) {
        if (millis() - lastOk > 100) {
            Serial.println("Sending OK");
            Serial2.println("OK");
            lastOk = millis();
        }
        if (Serial2.available()) {
            msg = Serial2.readStringUntil('\n');
            msg.trim();
            Serial.printf("RX MSG: [%s]\n", msg.c_str());
            if (msg == "handshake finale") {
                Serial.println("Received handshake finale");
                break;
            }
        }
        delay(10);
    }

    if (msg == "handshake finale") {
        Serial.println("Sending handshake OK");
        Serial2.println("Handshake ok");
        delay(50);
        msg = Serial2.readStringUntil('\n');
        msg.trim();
        Serial.printf("Received second msg: [%s]\n", msg.c_str());

        if (msg == "trusted") {
            Serial.println("Sending trusted");
            Serial2.println("trusted");

            String baudCmd = "";
            unsigned long baudStart = millis();
            while (millis() - baudStart < 1000) {
                if (Serial2.available()) {
                    baudCmd = Serial2.readStringUntil('\n');
                    baudCmd.trim();
                    break;
                }
                delay(10);
            }

            if (baudCmd.startsWith("BAUD=")) {
                uint32_t newBaud = baudCmd.substring(5).toInt();
                const uint32_t validBauds[] = {9600,19200,38400,57600,115200,230400,460800,921600,1000000,1500000,2000000,3000000,4000000};
                bool valid = false;
                for (uint32_t b : validBauds) {
                    if (newBaud == b) { valid = true; break; }
                }
                if (valid) {
                    Serial.printf("Switching to baudrate: %d\n", newBaud);
                    unsigned long ackStart = millis();
                    while (millis() - ackStart < 500) {
                        Serial2.println("baudrate received");
                        delay(50);
                    }
                    Serial2.end();
                    Serial2.begin(newBaud, SERIAL_8N1, rxEsp, txEsp);
                    uartBaud = newBaud;

                    Preferences prefsCfg;
                    prefsCfg.begin("uart_config", false);
                    prefsCfg.putUInt("baud", uartBaud);
                    prefsCfg.putInt("rx", rxEsp);
                    prefsCfg.putInt("tx", txEsp);
                    prefsCfg.end();

                    handshakeDone = true;
                    return;
                } else {
                        Serial.println("Invalid baudrate, sending unknown");
                        Serial2.println("baudrate unknown");
                        handshakeDone = false;
                        Serial2.end();
                        return;
                }
            } else {
                Serial.println("No BAUD command, handshake failed");
                rxEsp = -1;
                txEsp = -1;
                Serial2.end();
                handshakeDone = false;
                return;
            }
        } else {
            Serial.println("Handshake failed during TRUSTED phase");
            handshakeDone = false;
            Serial2.end();
            return;
        }
    } else {
        Serial.println("Handshake failed waiting FINAL");
        handshakeDone = false;
        Serial2.end();
        return;
    }

    delay(100);
}

void uartScanBegin() {

  Serial.begin(115200);
  delay(2000);

  Serial.println();
  Serial.println("==================================");
  Serial.println("ESP32 UART Manual Discovery");
  Serial.println("==================================");
  Serial.println();

  numDigital = getDigitalPins();

  if (numDigital == 0)
  {
    Serial.println("No digital pin database found.");
  }

  Serial.print("Digital pins loaded: ");
  for (int i = 0; i < numDigital; i++) {
    Serial.printf("%d ", digitalPins[i]);
  }
  Serial.println();
  Serial.printf("Fixed baud: %d\n", FIXED_BAUD);
  Serial.println();

  Preferences prefsCfg;
  prefsCfg.begin("uart_config", true);
  bool hasRx   = prefsCfg.isKey("rx");
  bool hasTx   = prefsCfg.isKey("tx");
  bool hasBaud = prefsCfg.isKey("baud");
  int nvsRx = prefsCfg.getInt("rx", -1);
  int nvsTx = prefsCfg.getInt("tx", -1);
  uint32_t nvsBaud = prefsCfg.getUInt("baud", FIXED_BAUD);
  bool manualPinsConfigured = (manualUartRxPin >= 0 && manualUartTxPin >= 0);
  bool manualMatchesNvs = (manualPinsConfigured && nvsRx == manualUartRxPin && nvsTx == manualUartTxPin);
  bool manualDiffersFromNvs = (manualPinsConfigured && (nvsRx != manualUartRxPin || nvsTx != manualUartTxPin));

  if (manualPinsConfigured && !manualMatchesNvs) {
    Preferences prefsWrite;
    prefsWrite.begin("uart_config", false);
    prefsWrite.putInt("rx", manualUartRxPin);
    prefsWrite.putInt("tx", manualUartTxPin);
    prefsWrite.remove("baud");
    prefsWrite.end();
    hasRx = true;
    hasTx = true;
    hasBaud = false;
    nvsRx = manualUartRxPin;
    nvsTx = manualUartTxPin;
    uartBaud = FIXED_BAUD;
  }

  if (hasRx && hasTx && hasBaud) {
    rxEsp = nvsRx;
    txEsp = nvsTx;
    uartBaud = nvsBaud;
    handshakeDone = true;
  }

  if (hasRx && hasTx && !hasBaud)
{
    rxEsp = nvsRx;
    txEsp = nvsTx;

    Serial.println("RX/TX found, but baud is missing.");
    Serial.println("Entering SKIP_DISCOVERY mode.");

    Serial2.begin(FIXED_BAUD, SERIAL_8N1, rxEsp, txEsp);

    unsigned long start = millis();
    unsigned long lastSkip = 0;

    while (millis() - start < 5000)
    {
        if (millis() - lastSkip > 100)
        {
            Serial2.println("SKIP_DISCOVERY");
            lastSkip = millis();
        }

        if (Serial2.available())
        {
            String msg = Serial2.readStringUntil('\n');
            msg.trim();

            if (msg == "handshake finale")
            {
                performHandshake();
                break;
            }
        }

        delay(10);
    }

    return;
  }

  if (handshakeDone) {
    Serial2.end();
    delay(10);
    Serial2.begin(FIXED_BAUD, SERIAL_8N1, rxEsp, txEsp);
    delay(100);

    Serial.println("Sending SKIP_DISCOVERY to the Pico...");
    unsigned long skipStart = millis();
    unsigned long lastSkip = 0;

    while (millis() - skipStart < 2000)
  {
    if (millis() - lastSkip > 100)
    {
        Serial2.println("SKIP_DISCOVERY");
        lastSkip = millis();
    }

    if (Serial2.available())
    {
        String confirmMsg =
            Serial2.readStringUntil('\n');

        confirmMsg.trim();

        if (confirmMsg == "handshake finale")
        {
            performHandshake();
            return;
        }
    }

      delay(5);
    }
  }
}

void uartScanLoop() {
  if (handshakeDone) {
    static unsigned long startCheck = 0;
    static bool checkPending = true;
    if (checkPending && startCheck == 0) {
        startCheck = millis();
        Serial.println("Starting 2-second initial check for Pico discovery mode.");
    }

    static uint8_t prevByte = 0;
    while (Serial2.available()) {
        uint8_t data = Serial2.read();
        Serial.printf("[RX] 0x%02X", data);
        if (data >= 32 && data <= 126) Serial.printf(" ('%c')", data);
        Serial.println();

        if (checkPending && prevByte == 0x54 && data == 0x58) {
            Serial.println("Pico is in discovery mode (within 3s); restarting handshake...");
            handshakeDone = false;
            checkPending = false;
            rxEsp = -1;
            txEsp = -1;
            Serial2.end();
            return;
        }
        prevByte = data;
    }

    if (checkPending && (millis() - startCheck > 2000)) {
        checkPending = false;
        Serial.println("Initial check completed. Pico is operational.");
    }
    return;
  }

  if (rxEsp == -1) {
    Serial.println();
    Serial.println("=== SEARCHING TX PATTERN ===");
    for (int i = 0; i < numDigital; i++) {
      int pin = digitalPins[i];
      Serial.printf("Scanning pin %d for TX...\n", pin);
      if (receivePattern(pin, 0x54, 0x58, FIXED_BAUD, 300)) {
        rxEsp = pin;
        Serial.printf("FOUND TX on pin %d -> RX_ESP\n", rxEsp);
        break;
      }
    }
  }

  else if (txEsp == -1) {
    static bool phase2Started = false;
    static int currentPinIndex = 0;
    static unsigned long phase2Start = 0;

    if (!phase2Started) {
      Serial.println();
      Serial.println("=== SEARCHING RX PATTERN ===");
      phase2Started = true;
      currentPinIndex = 0;
    }

    if (currentPinIndex >= numDigital) {
      Serial.println("RX not found; retrying RX search");
      currentPinIndex = 0;
      phase2Start = 0;
      delay(50);
      return;
    }

    int pin = digitalPins[currentPinIndex];
    if (pin == rxEsp) {
      currentPinIndex++;
      return;
    }

    if (phase2Start == 0) {
      phase2Start = millis();
      pinMode(pin, INPUT_PULLUP);
      Serial.printf("Listening on pin %d for RX...\n", pin);
    }

    int lowTransitions = 0;
    unsigned long detectStart = micros();
    while (micros() - detectStart < 200000) {
      if (gpio_get_level((gpio_num_t)pin) == LOW) {
        lowTransitions++;
        Serial.printf("[PIN %d] LOW DETECTED (%d)\n", pin, lowTransitions);
        delayMicroseconds(150);
      }
      delayMicroseconds(5);
    }

    if (lowTransitions >= 3) {
      txEsp = pin;
      Serial.printf("FOUND RX ACTIVITY on pin %d -> TX_ESP\n", txEsp);
      phase2Started = false;
      phase2Start = 0;
      return;
    }

    if (millis() - phase2Start > 2000) {
      Serial.printf("Pin %d timeout\n", pin);
      currentPinIndex++;
      phase2Start = 0;
    }
  }
  else {
    performHandshake();
  }
}

bool uartHandshakeReady() {
return handshakeDone;
}

int uartGetRxPin() {
return rxEsp;
}

int uartGetTxPin() {
return txEsp;
}

uint32_t uartGetBaud() {
return uartBaud;
}

bool uartScanReady() {
    return handshakeDone;
}