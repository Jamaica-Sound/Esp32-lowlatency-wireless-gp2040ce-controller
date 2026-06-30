#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSV2_SYNC 0x534A
#define JSV2_TYPE_CONFIG  0x01
#define JSV2_TYPE_RUNTIME 0x02
#define JSV2_MAX_PINS 64

#pragma pack(push, 1)

typedef struct {
    uint16_t sync;
    uint8_t type;
    uint8_t digitalCount;
    uint8_t analogCount;
    uint8_t pins[JSV2_MAX_PINS];

} JSV2_ConfigPacket;

typedef struct {
    uint16_t sync;
    uint8_t type;
    uint64_t digitalBits;

} JSV2_RuntimePacket;

#pragma pack(pop)

static inline uint16_t jsv2_crc16(
    const uint8_t *data,
    uint16_t len
) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            }
            else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static inline uint16_t jsv2_config_size(
    uint8_t digitalCount,
    uint8_t analogCount
) {

    return
        2 +     // JS
        1 +     // TYPE
        1 +     // digitalCount
        1 +     // analogCount
        digitalCount +
        analogCount +
        2;      // CRC
}

static inline uint16_t jsv2_runtime_size(
    uint8_t analogCount
) {

    return
        2 +     // JS
        1 +     // TYPE
        8 +     // digitalBits
        (analogCount * sizeof(uint16_t))
        + 2;    // CRC
}

#ifdef __cplusplus
}
#endif