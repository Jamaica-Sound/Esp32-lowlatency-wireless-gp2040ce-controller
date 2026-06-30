#pragma once

#include <stdint.h>

extern uint32_t manualPacingUs;

bool runtimeInit();
void runtimeLoop();