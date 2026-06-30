#include "pin_scan.h"
#include <Arduino.h>
#include <Preferences.h>

#define MAX_PINS 64
#define DEEP_SAMPLES 96
#define RETRY_COUNT 3

Preferences prefs;
const char* NVS_NAMESPACE = "pin_scanner";
const char* KEY_SCANNED = "scannedMask";
const char* KEY_CURRENT_PIN = "currentPin";
const char* KEY_PHASE = "phase";
const char* KEY_STATE = "st_";
const char* KEY_MINV = "min_";
const char* KEY_MAXV = "max_";
const char* KEY_AVG = "avg_";
const char* KEY_DRIFT = "drift_";
const char* KEY_DIGITAL_COUNT = "digCount";
const char* KEY_ANALOG_COUNT  = "anaCount";
const char* KEY_DIGITAL_LIST  = "digList";
const char* KEY_ANALOG_LIST   = "anaList";

uint64_t scannedMask = 0;
int currentPin = -1;
uint8_t phase = 0;

enum PinState {
  PIN_UNKNOWN,
  PIN_DIGITAL,
  PIN_ANALOG,
  PIN_FLOATING,
  PIN_UNSAFE,
  PIN_NON_EXISTENT,
  PIN_EMPTY
};

struct ADCStats {
  uint16_t minv, maxv, avg, live;
  uint32_t variance, drift;
};

struct PinResult {
  PinState state;
  ADCStats stats;
  int retries;
};

PinResult results[MAX_PINS];
uint64_t usableMask = 0, analogMask = 0;

bool isAnalogReadable(int pin) {
  return (pin >= 0 && pin <= 39);
}

bool safeAnalogRead(int pin, int &value) {
  if (!isAnalogReadable(pin)) return false;
  uint32_t t0 = micros();
  int v = analogRead(pin);
  if (micros() - t0 > 8000) return false;
  if (v < 0 || v > 4095) return false;
  value = v;
  return true;
}

ADCStats analyzeADC(int pin) {
  ADCStats s{};
  s.minv = 4095; s.maxv = 0;
  uint64_t sum = 0, sq = 0;
  int prev = -1;
  for (int i = 0; i < DEEP_SAMPLES; i++) {
    int v;
    if (!safeAnalogRead(pin, v)) continue;
    s.live = v;
    if (v < s.minv) s.minv = v;
    if (v > s.maxv) s.maxv = v;
    sum += v; sq += (uint64_t)v * v;
    if (prev >= 0) s.drift += abs(v - prev);
    prev = v;
    delayMicroseconds(25);
  }
  if (s.maxv == 0 && s.minv == 4095) {
    s.minv = s.maxv = s.avg = s.live = 0;
    s.drift = 0;
  } else {
    s.avg = sum / DEEP_SAMPLES;
    s.variance = (sq / DEEP_SAMPLES) - ((uint64_t)s.avg * s.avg);
  }
  return s;
}

bool pinSeemsAlive(int pin) {
  if (isAnalogReadable(pin)) {
    for (int i = 0; i < 6; i++) {
      int v;
      if (safeAnalogRead(pin, v)) return true;
      delay(1);
    }
  }
  pinMode(pin, INPUT);
  delay(2);
  int a = digitalRead(pin);
  return (a == 0 || a == 1);
}

bool runtimeProbe(int pin) {
  uint32_t t0 = millis();
  pinMode(pin, INPUT);
  delay(1);
  int a = digitalRead(pin);
  delay(1);
  int v = 0;
  bool analogOk = safeAnalogRead(pin, v);
  if (millis() - t0 > 1200) return false;
  if (!analogOk && !(a == 0 || a == 1)) return false;
  return true;
}

void savePinResultToNVS(int pin) {
  char key[20];
  snprintf(key, sizeof(key), "%s%d", KEY_STATE, pin);
  prefs.putUChar(key, (uint8_t)results[pin].state);
  snprintf(key, sizeof(key), "%s%d", KEY_MINV, pin);
  prefs.putUShort(key, results[pin].stats.minv);
  snprintf(key, sizeof(key), "%s%d", KEY_MAXV, pin);
  prefs.putUShort(key, results[pin].stats.maxv);
  snprintf(key, sizeof(key), "%s%d", KEY_AVG, pin);
  prefs.putUShort(key, results[pin].stats.avg);
  snprintf(key, sizeof(key), "%s%d", KEY_DRIFT, pin);
  prefs.putUInt(key, results[pin].stats.drift);
}

void loadPinResultFromNVS(int pin) {
  char key[20];
  snprintf(key, sizeof(key), "%s%d", KEY_STATE, pin);
  results[pin].state = (PinState)prefs.getUChar(key, PIN_UNKNOWN);
  snprintf(key, sizeof(key), "%s%d", KEY_MINV, pin);
  results[pin].stats.minv = prefs.getUShort(key, 0);
  snprintf(key, sizeof(key), "%s%d", KEY_MAXV, pin);
  results[pin].stats.maxv = prefs.getUShort(key, 0);
  snprintf(key, sizeof(key), "%s%d", KEY_AVG, pin);
  results[pin].stats.avg = prefs.getUShort(key, 0);
  snprintf(key, sizeof(key), "%s%d", KEY_DRIFT, pin);
  results[pin].stats.drift = prefs.getUInt(key, 0);
}

bool testDigitalPull(int pin) {

    pinMode(pin, INPUT_PULLUP);
    delay(3);
    int up = digitalRead(pin);
    pinMode(pin, INPUT_PULLDOWN);
    delay(3);
    int down = digitalRead(pin);
    pinMode(pin, INPUT);
    bool ideal = (up == 1 && down == 0);
    bool differential = (up != down);
    return ideal || differential;
}

uint8_t testDigitalPullScore(int pin) {
    uint8_t score = 0;
    pinMode(pin, INPUT_PULLUP);
    delay(3);
    int up = digitalRead(pin);
    pinMode(pin, INPUT_PULLDOWN);
    delay(3);
    int down = digitalRead(pin);
    pinMode(pin, INPUT);
    delay(1);

    if (up == 1 && down == 0) {
        score += 2;
    }

    if (up == 0 && down == 1) {
        score += 2;
    }

    if (up != down) {
        score += 1;
    }

    if (up == down) {
        score += 0;
    }

    return score;
}

void phase0_lightScan() {
  for (int pin = 0; pin < MAX_PINS; pin++) {
    if (scannedMask & (1ULL << pin)) continue;

    currentPin = pin;
    prefs.putInt(KEY_CURRENT_PIN, currentPin);
    prefs.end();
    prefs.begin(NVS_NAMESPACE, false);

    PinResult &r = results[pin];
    r.state = PIN_UNKNOWN;
    r.retries = 0;

    if (!pinSeemsAlive(pin)) {
      r.state = PIN_NON_EXISTENT;
      savePinResultToNVS(pin);
      scannedMask |= (1ULL << pin);
      prefs.putULong64(KEY_SCANNED, scannedMask);
      currentPin = -1;
      prefs.putInt(KEY_CURRENT_PIN, currentPin);
      continue;
    }

    bool runtimeOK = false;
    for (int i = 0; i < RETRY_COUNT; i++) {
      if (runtimeProbe(pin)) { runtimeOK = true; break; }
      r.retries++;
      delay(10);
    }

    if (!runtimeOK) {
      r.state = PIN_UNSAFE;
      savePinResultToNVS(pin);
      scannedMask |= (1ULL << pin);
      prefs.putULong64(KEY_SCANNED, scannedMask);
      currentPin = -1;
      prefs.putInt(KEY_CURRENT_PIN, currentPin);
      continue;
    }

    if (!isAnalogReadable(pin)) {
      pinMode(pin, INPUT);
      delay(2);
      int val = digitalRead(pin);
      if (val == 1) {
        r.state = PIN_DIGITAL;
      } else {
        r.state = PIN_EMPTY;
      }
      r.stats.minv = r.stats.maxv = r.stats.avg = 0;
      r.stats.drift = 0;
      usableMask |= (1ULL << pin);
      savePinResultToNVS(pin);
      scannedMask |= (1ULL << pin);
      prefs.putULong64(KEY_SCANNED, scannedMask);
      currentPin = -1;
      prefs.putInt(KEY_CURRENT_PIN, currentPin);
      continue;
    }

    ADCStats s = analyzeADC(pin);
    r.stats = s;

    bool floating =
    ((s.maxv - s.minv) > 150 && s.drift > 300) ||
    ((s.maxv - s.minv) < 120 && s.drift > 1000 && !(s.avg > 1750 && s.avg < 2250));

    const int mid = 4095 / 2;
    const int tol = 300;

bool analogStable =
    (s.minv >= mid - tol && s.minv <= mid + tol) &&
    (s.avg  >= mid - tol && s.avg  <= mid + tol) &&
    (s.maxv >= mid - tol && s.maxv <= mid + tol);
    
    if (analogStable) {
    r.state = PIN_ANALOG;
    }
      else if (floating) {
      r.state = PIN_FLOATING;
    }
      else if (s.avg > 4000 && (s.maxv - s.minv) < 8) {
      r.state = PIN_DIGITAL;
    } else if (s.avg < 10 && (s.maxv - s.minv) < 8) {
      r.state = PIN_EMPTY;
    } else {
      r.state = PIN_UNKNOWN;
    }

    if (r.state == PIN_FLOATING || r.state == PIN_ANALOG || r.state == PIN_DIGITAL) {
      usableMask |= (1ULL << pin);
      if (r.state == PIN_ANALOG) analogMask |= (1ULL << pin);
    }

    savePinResultToNVS(pin);
    scannedMask |= (1ULL << pin);
    prefs.putULong64(KEY_SCANNED, scannedMask);
    currentPin = -1;
    prefs.putInt(KEY_CURRENT_PIN, currentPin);
  }
}

void phase1_pullTest() {
  Serial.println("\nPhase 2: pull-up/pull-down test on candidate DIGITAL pins...");
  for (int pin = 0; pin < MAX_PINS; pin++) {
    if (!(scannedMask & (1ULL << pin))) continue;
    loadPinResultFromNVS(pin);
    if (results[pin].state != PIN_DIGITAL) continue;
    if (results[pin].state == PIN_UNSAFE) continue;

    currentPin = pin;
    prefs.putInt(KEY_CURRENT_PIN, currentPin);
    prefs.end();
    prefs.begin(NVS_NAMESPACE, false);

uint8_t score = testDigitalPullScore(pin);

if (score <= 1) {
    results[pin].state = PIN_EMPTY;
}
else if (score <= 3) {
    results[pin].state = PIN_UNKNOWN;
}
else {
    results[pin].state = PIN_DIGITAL;
}

    currentPin = -1;
    prefs.putInt(KEY_CURRENT_PIN, currentPin);
    delay(10);
  }
}

void printFinalReport() {
  Serial.println("\n========== FINAL SUMMARY ==========");
  Serial.println("PIN | STATE           | MIN  | MAX  | AVG  | DRIFT | NOTES");
  for (int pin = 0; pin < MAX_PINS; pin++) {
    loadPinResultFromNVS(pin);
    PinResult &r = results[pin];
    const char* stateStr = "UNKNOWN";
    const char* notes = "";

    switch (r.state) {
      case PIN_DIGITAL:      stateStr = "DIGITAL"; break;
      case PIN_ANALOG:       stateStr = "ANALOG"; break;
      case PIN_FLOATING:     stateStr = "FLOATING"; break;
      case PIN_UNSAFE:       stateStr = "UNSAFE"; notes = "(skipped)"; break;
      case PIN_NON_EXISTENT: stateStr = "NON_EXISTENT"; break;
      case PIN_EMPTY:        stateStr = "EMPTY"; notes = "(nothing connected)"; break;
      default:               stateStr = "UNKNOWN"; break;
    }

    if (r.state == PIN_UNSAFE || r.state == PIN_NON_EXISTENT || r.state == PIN_EMPTY) {
      Serial.printf("%3d | %-14s |      |      |      |       | %s\n", pin, stateStr, notes);
    } else {
      Serial.printf("%3d | %-14s | %4u | %4u | %4u | %5u | %s\n",
        pin, stateStr, r.stats.minv, r.stats.maxv, r.stats.avg, r.stats.drift, notes);
    }
  }
  Serial.println("===================================\n");
}

void buildRuntimeLists() {

  uint8_t digitalList[64];
  uint8_t analogList[16];

  uint8_t digitalCount = 0;
  uint8_t analogCount = 0;

  for (int pin = 0; pin < MAX_PINS; pin++) {

    loadPinResultFromNVS(pin);

    if (results[pin].state == PIN_DIGITAL) {
      digitalList[digitalCount++] = pin;
    }
    else if (results[pin].state == PIN_ANALOG) {
      analogList[analogCount++] = pin;
    }
  }

  prefs.putUChar(KEY_DIGITAL_COUNT, digitalCount);
  prefs.putBytes(KEY_DIGITAL_LIST, digitalList, digitalCount);

  prefs.putUChar(KEY_ANALOG_COUNT, analogCount);
  prefs.putBytes(KEY_ANALOG_LIST, analogList, analogCount);

  Serial.println("Runtime pin lists saved.");

  Serial.printf("DIGITAL COUNT: %d\n", digitalCount);
  Serial.printf("ANALOG COUNT : %d\n", analogCount);
}

void runPinScanIfNeeded() {
  delay(1500);
  analogReadResolution(12);
  Serial.println("\nESP32 SCANNER WITH TWO-PHASE PULL TEST\n");

  prefs.begin(NVS_NAMESPACE, false);
  scannedMask = prefs.getULong64(KEY_SCANNED, 0);
  currentPin = prefs.getInt(KEY_CURRENT_PIN, -1);
  phase = prefs.getUChar(KEY_PHASE, 0);

  if (currentPin >= 0 && currentPin < MAX_PINS && !(scannedMask & (1ULL << currentPin))) {
    Serial.printf("⚠️ Pin %d caused a reset during phase %d. It is marked as UNSAFE and skipped.\n", currentPin, phase);
    results[currentPin].state = PIN_UNSAFE;
    savePinResultToNVS(currentPin);
    scannedMask |= (1ULL << currentPin);
    prefs.putULong64(KEY_SCANNED, scannedMask);
    currentPin = -1;
    prefs.putInt(KEY_CURRENT_PIN, currentPin);
  }

  if (phase == 0) {
    Serial.println("PHASE 0: light scan...");
    phase0_lightScan();
    Serial.println("PHASE 1: pull-up/pull-down test on DIGITAL pins...");
    phase1_pullTest();
    buildRuntimeLists();
    Serial.println("Final report:");
    printFinalReport();
    prefs.putUChar(KEY_PHASE, 2);
    prefs.end();
    Serial.println("Scan completed.");
    ESP.restart();
  }

  else if (phase == 2) {
    Serial.println("Scan already completed.");
    printFinalReport();
    prefs.end();
    return;
  }

  else {
    prefs.clear();
    prefs.end();
    Serial.println("State corrupted; NVS reset.");
    return;
  }
}