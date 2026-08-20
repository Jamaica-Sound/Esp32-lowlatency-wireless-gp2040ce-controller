#ifndef LATENCY_TEST_H
#define LATENCY_TEST_H

#include <Arduino.h>
#include <esp_now.h>

extern bool enableLatencyTest;

bool latencyTestHandleRecv(const esp_now_recv_info *info, const uint8_t *data, int len);

#endif