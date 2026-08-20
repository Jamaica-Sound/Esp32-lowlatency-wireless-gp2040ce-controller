#include "latency_test.h"

struct __attribute__((packed)) LatencyPacket {

    uint32_t magic;     
    uint32_t timestamp; 
};

bool latencyTestHandleRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    if (!enableLatencyTest) return false; 
    
    if (len == sizeof(LatencyPacket)) {
        const LatencyPacket* pkt = (const LatencyPacket*)data;
        if (pkt->magic == 0x4C415421) {
            esp_now_send(info->src_addr, data, len); // Immediate hardware rebound
            return true; 
        }
    }
    return false; 
}