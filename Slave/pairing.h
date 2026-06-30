#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t peerMac[6];
extern uint8_t manualPeerMac[6];
extern int8_t manualChannel;

void pairingBegin();
void pairingLoop();
bool pairingReady();
bool isPaired();

#ifdef __cplusplus
}
#endif