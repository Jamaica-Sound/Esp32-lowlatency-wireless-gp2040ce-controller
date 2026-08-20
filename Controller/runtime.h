#pragma once

#include <stdint.h>

extern const char* manualRotaryPins;
extern uint32_t manualPacingUs;

bool runtimeInit();
void runtimeLoop();