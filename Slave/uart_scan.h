#pragma once 

#ifdef __cplusplus
extern "C" {
#endif

extern int manualUartRxPin;
extern int manualUartTxPin;
extern int manualUartBaud;

void uartScanBegin();
void uartScanLoop();
bool uartScanReady();

#ifdef __cplusplus
}
#endif