#pragma once

#include <stdint.h>

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

extern bool wifiScanWaitingForBestChannel;

void wifiScanHandleBestChannel(const uint8_t *data, int len);
uint8_t channelSelectOrApply();

void channelScanHandleAck(const uint8_t *data, int len);

extern bool channelScanActive;

#ifdef __cplusplus
}
#endif