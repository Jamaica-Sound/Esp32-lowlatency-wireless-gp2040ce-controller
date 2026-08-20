#include "latency_test.h"
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"

extern uint8_t peerMac[6];

struct __attribute__((packed)) LatencyPacket {
    uint32_t magic;
    uint32_t timestamp;
};

volatile uint64_t totalLatencyRttUs = 0;
volatile uint32_t latencyPacketCount = 0;
static uint32_t lastLatencyPrintTimeMs = 0;
static uint32_t calculatedIntervalUs = 20000;

static void latencyTask(void *pvParameters);

void latencyTestInit() {
    if (!enableLatencyTest) return;

    if (latencyPacketsPerSecond > 0) {
        calculatedIntervalUs = 1000000 / latencyPacketsPerSecond;
    }

    esp_now_register_recv_cb(OnLatencyDataRecv);
}

void latencyTestStart() {
    if (!enableLatencyTest) return;

    xTaskCreatePinnedToCore(
        latencyTask,
        "latencyTask",
        4096,
        NULL,
        4,
        NULL,
        0
    );
}

void OnLatencyDataRecv(const esp_now_recv_info_t * recvInfo, const uint8_t *incomingData, int len) {
    if (!enableLatencyTest) return;
    
    if (len == sizeof(LatencyPacket)) {
        LatencyPacket* pkt = (LatencyPacket*)incomingData;
        if (pkt->magic == 0x4C415421) { 
            uint32_t currentTimeUs = (uint32_t)esp_timer_get_time();
            totalLatencyRttUs += (currentTimeUs - pkt->timestamp);
            latencyPacketCount++;
        }
    }
}

static void latencyTask(void *pvParameters) {
    // Remove manual TWDT registration: let FreeRTOS breathe normally
    uint32_t lastPingTimeUs = micros();

    while (1) {
        // Check if the peer MAC is still empty (waiting for active pairing)
        if (peerMac[0] == 0 && peerMac[1] == 0 && peerMac[2] == 0 && 
            peerMac[3] == 0 && peerMac[4] == 0 && peerMac[5] == 0) {
            vTaskDelay(pdMS_TO_TICKS(50)); 
            lastPingTimeUs = micros();     
            continue;
        }

        uint32_t currentUs = micros();
        
        // Pure time comparison
        if (currentUs - lastPingTimeUs >= calculatedIntervalUs) {
            lastPingTimeUs += calculatedIntervalUs;

            LatencyPacket pingPkt;
            pingPkt.magic = 0x4C415421;
            pingPkt.timestamp = (uint32_t)esp_timer_get_time();
            
            esp_now_send(peerMac, (uint8_t*)&pingPkt, sizeof(pingPkt));
        }
        
        // =========================================================================
        // THE SECRET TO MINIMAL LATENCY: EFFECTIVE TICK RELEASE
        // Pause the task for the fraction of milliseconds required by the calculation.
        // If we calculate the dynamic interval in ticks, we give IDLE0 time to clean the radio.
        // The antenna will remain fresh and reactive, processing Ping-Pong in 0.25 ms.
        // =========================================================================
        uint32_t delayTicks = pdMS_TO_TICKS(calculatedIntervalUs / 1000);
        if (delayTicks == 0) delayTicks = 1; // Minimum 1 Tick safety margin to feed IDLE0
        
        vTaskDelay(delayTicks); 
    }
}


void latencyTestLoopInject() {
    if (!enableLatencyTest) return;

    uint32_t latNowMs = millis();
    if (latNowMs - lastLatencyPrintTimeMs >= 1000) {
        lastLatencyPrintTimeMs = latNowMs;
        if (latencyPacketCount > 0) {
            noInterrupts();
            uint32_t count = latencyPacketCount;
            uint64_t totalRtt = totalLatencyRttUs;
            latencyPacketCount = 0; 
            totalLatencyRttUs = 0;
            interrupts();

            float avgRttUs = (float)totalRtt / count;
            float avgOneWayMs = (avgRttUs / 2.0) / 1000.0;
            
            Serial.printf("[LATENCY] Packets received/s: %d | Average RTT: %.1f us | 1-Way Latency: %.3f ms\n", 
                          count, avgRttUs, avgOneWayMs);
        }
    }
}