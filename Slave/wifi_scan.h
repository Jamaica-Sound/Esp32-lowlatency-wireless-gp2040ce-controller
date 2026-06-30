#pragma once

#include <stdint.h>
#include <esp_now.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t magic;
    uint8_t newChannel;
    uint32_t testDuration;
} SyncPacket;

typedef struct {
    uint32_t magic;
    uint8_t channel;
    uint8_t status;
} AckPacket;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t bestChannel;
    uint16_t packetCount;
    uint8_t status;
} BestChannelPacket;

void wifiScanRecv(const esp_now_recv_info *info, const uint8_t *data, int len);
uint8_t wifiChannelSelectOrApply();
void wifiScanLoop();
void wifiScanHandleAck(const uint8_t *data, int len);

extern bool wifiScanComplete;
extern bool wifiScanWaitingForBestChannel;

#ifdef __cplusplus
}
#endif