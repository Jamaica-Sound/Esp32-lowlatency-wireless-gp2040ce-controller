#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char* manualDigitalPins;
extern const char* manualAnalogPins;

void runPinScanIfNeeded();

#ifdef __cplusplus
}
#endif