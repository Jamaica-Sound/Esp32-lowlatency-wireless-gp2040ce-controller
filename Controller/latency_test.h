#ifndef LATENCY_TEST_H
#define LATENCY_TEST_H

#include <Arduino.h>
#include <esp_now.h>

extern bool enableLatencyTest;
extern uint32_t latencyPacketsPerSecond;

void latencyTestInit();
void latencyTestStart();
void latencyTestLoopInject();

void OnLatencyDataRecv(const esp_now_recv_info_t * recvInfo, const uint8_t *incomingData, int len);

#endif