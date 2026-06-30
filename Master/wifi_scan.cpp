#include "wifi_scan.h"
#include "pairing.h"
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>

extern uint8_t peerMac[6];
extern int8_t manualChannel;
extern const uint32_t testDurationMs;
extern const uint32_t pktIntervalUs;

bool channelScanActive = false;

typedef struct {
    uint8_t channel;
    uint32_t sent;
    uint32_t acked;
    uint32_t lost;
    float throughput;
    float score;
} TestResult;

static TestResult results[14];

static uint32_t packetId = 0;
static uint32_t ackCount = 0;
static bool ackReceived = false;
static uint32_t startTime = 0;
static uint32_t sentCount = 0;
static uint32_t testStartMs = 0;
static uint32_t lastSendTime = 0;
static bool syncAckReceived = false;
static uint8_t receivedBestChannel = 0;
static bool bestChannelReceived = false;
bool wifiScanWaitingForBestChannel = false;

void wifiScanHandleBestChannel(const uint8_t *data, int len) {
    Serial.printf("[WIFI] received len=%d\n", len);
    if (len != sizeof(BestChannelPacket)) return;
    BestChannelPacket *pkt = (BestChannelPacket*)data;
    if (pkt->magic == 0xDEADBEEF && pkt->status == 0) {
        receivedBestChannel = pkt->bestChannel;
        uint16_t packetCount = pkt->packetCount;
        bestChannelReceived = true;
        Serial.printf("[WIFI] Best channel received: %d with %d packets\n", receivedBestChannel, packetCount);

        uint32_t periodo_us = (testDurationMs * 100000) / (packetCount * 95);
        if (periodo_us < 200) periodo_us = 200;
        if (periodo_us > 5000) periodo_us = 5000;

        Preferences prefs;
        prefs.begin("espnow", false);
        prefs.putUInt("pacing_us", periodo_us);
        prefs.end();

        Serial.printf("[WIFI] New pacing period calculated: %d µs\n", periodo_us);
    }
}

static void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ackReceived = true;
        ackCount++;
    }
}

static void sendEndCommand() {
    SyncPacket end;
    end.magic = 0xDEADBEEF;
    end.newChannel = 0;
    end.testDuration = 0;
    for (int i = 0; i < 5; i++) {
        esp_now_send(peerMac, (uint8_t*)&end, sizeof(end));
        delay(50);
    }
    Serial.println("[SCAN] Comando fine test inviato.");
}

void channelScanHandleAck(const uint8_t *data, int len) {
    if (len == sizeof(AckPacket)) {
        AckPacket *ack = (AckPacket*)data;
        if (ack->magic == 0xDEADBEEF && ack->status == 0) {
            syncAckReceived = true;
            Serial.printf("[SCAN] Sync ACK from slave for channel %d\n", ack->channel);
        }
    }
}

static bool sendSyncAndWaitAck(uint8_t newChannel) {
    SyncPacket sync;
    sync.magic = 0xDEADBEEF;
    sync.newChannel = newChannel;
    sync.testDuration = testDurationMs;

    syncAckReceived = false;

    Serial.printf("[SCAN] Sending sync for channel %d...\n", newChannel);

    while (!syncAckReceived) {
        esp_now_send(peerMac, (uint8_t*)&sync, sizeof(sync));
        delay(150);
    }

    Serial.printf("[SCAN] Slave confirmed on channel %d\n", newChannel);
    return true;
}

static void updatePeerChannel(uint8_t *mac, uint8_t channel) {
    esp_now_del_peer(mac);
    delay(20);
    esp_now_peer_info_t peerInfo;
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = channel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peerInfo);
    if (err != ESP_OK) {
        Serial.printf("[SCAN] Error adding peer on channel %d: %d\n", channel, err);
    }
    delay(20);
}

static TestResult testChannel(uint8_t channel) {
    TestResult result = {0};
    result.channel = channel;

    Serial.printf("[SCAN] Testing channel %d...\n", channel);

    if (!sendSyncAndWaitAck(channel)) {
        return result;
    }

    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    delay(50);
    updatePeerChannel(peerMac, channel);
    delay(150);

    ackCount = 0;
    sentCount = 0;
    testStartMs = millis();
    lastSendTime = micros();

    while (millis() - testStartMs < testDurationMs) {
        uint32_t now = micros();
        if (now - lastSendTime >= pktIntervalUs) {
            uint8_t testData[16];
            packetId++;
            memcpy(testData, &packetId, sizeof(packetId));
            memcpy(testData + 4, &channel, 1);
            esp_now_send(peerMac, testData, sizeof(testData));
            sentCount++;
            lastSendTime = now;
        }
        delayMicroseconds(10);
    }

    delay(500);

    result.sent = sentCount;
    result.acked = ackCount;
    result.lost = sentCount - ackCount;
    result.throughput = (float)sentCount * 1000 / testDurationMs;
    result.score = (float)ackCount * 1000 / testDurationMs;

    Serial.printf("[SCAN] Channel %d: sent %d, ACK %d\n", channel, result.sent, result.acked);
    return result;
}

static int findBestChannel() {
    int best = 1;
    float bestScore = results[1].score;
    for (int ch = 2; ch <= 13; ch++) {
        if (results[ch].score > bestScore) {
            bestScore = results[ch].score;
            best = ch;
        }
    }
    return best;
}

uint8_t channelScanStart() {
    channelScanActive = true;
    esp_now_register_send_cb(onDataSent);

    Serial.println("[SCAN] Starting channel scan...");

    for (int ch = 1; ch <= 13; ch++) {
        results[ch] = testChannel(ch);
        delay(500);
    }

    sendEndCommand();

    wifiScanWaitingForBestChannel = true;
    bestChannelReceived = false;
    receivedBestChannel = 0;

    Serial.println("[SCAN] Waiting for the best channel from the slave...");

    while (!bestChannelReceived) {
        delay(50);
    }

    wifiScanWaitingForBestChannel = false;
    channelScanActive = false;

    BestChannelPacket ack;
    ack.magic = 0xDEADBEEF;
    ack.bestChannel = receivedBestChannel;
    ack.status = 0;
    esp_now_send(peerMac, (uint8_t*)&ack, sizeof(ack));
    Serial.printf("[SCAN] ACK sent to slave for channel %d\n", receivedBestChannel);

    uint8_t best = receivedBestChannel;
    Serial.printf("[SCAN] Best channel received: %d\n", best);

    Preferences prefs;
    prefs.begin("espnow", false);
    prefs.putUChar("best_channel", best);
    prefs.end();

    return best;
}

uint8_t channelSelectOrApply() {
    if (manualChannel > 0) {
        uint8_t ch = manualChannel;
        Serial.printf("[CHANNEL] Using manual channel: %d\n", ch);
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        bool isBroadcast = true;
        for (int i = 0; i < 6; i++) {
            if (peerMac[i] != 0xFF) {
                isBroadcast = false;
                break;
            }
        }
        if (!isBroadcast) {
            esp_now_del_peer(peerMac);
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerMac, 6);
            peerInfo.channel = ch;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return ch;
    }

    if (manualChannel == 0) {
        Serial.println("[CHANNEL] Using default channel: 1");
        esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
        bool isBroadcast = true;
        for (int i = 0; i < 6; i++) {
            if (peerMac[i] != 0xFF) {
                isBroadcast = false;
                break;
            }
        }
        if (!isBroadcast) {
            esp_now_del_peer(peerMac);
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerMac, 6);
            peerInfo.channel = 1;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return 1;
    }

    Serial.println("[CHANNEL] Automatic channel scan...");
    Preferences prefs;
    prefs.begin("espnow", true);
    uint8_t savedChannel = prefs.getUChar("best_channel", 0);
    prefs.end();

    if (savedChannel >= 1 && savedChannel <= 13) {
        Serial.printf("[CHANNEL] Saved channel found: %d\n", savedChannel);
        uint8_t ch = savedChannel;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        bool isBroadcast = true;
        for (int i = 0; i < 6; i++) {
            if (peerMac[i] != 0xFF) {
                isBroadcast = false;
                break;
            }
        }
        if (!isBroadcast) {
            esp_now_del_peer(peerMac);
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerMac, 6);
            peerInfo.channel = ch;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return ch;
    }

    uint8_t best = channelScanStart();
    if (best >= 1 && best <= 13) {
        Serial.printf("[CHANNEL] Best channel found: %d\n", best);
        Preferences prefs2;
        prefs2.begin("espnow", false);
        prefs2.putUChar("best_channel", best);
        prefs2.end();
        esp_wifi_set_channel(best, WIFI_SECOND_CHAN_NONE);
        bool isBroadcast = true;
        for (int i = 0; i < 6; i++) {
            if (peerMac[i] != 0xFF) {
                isBroadcast = false;
                break;
            }
        }
        if (!isBroadcast) {
            esp_now_del_peer(peerMac);
            esp_now_peer_info_t peerInfo;
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, peerMac, 6);
            peerInfo.channel = best;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return best;
    }

    Serial.println("[CHANNEL] Scan failed, using channel 1");
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    bool isBroadcast = true;
    for (int i = 0; i < 6; i++) {
        if (peerMac[i] != 0xFF) {
            isBroadcast = false;
            break;
        }
    }
    if (!isBroadcast) {
        esp_now_del_peer(peerMac);
        esp_now_peer_info_t peerInfo;
        memset(&peerInfo, 0, sizeof(peerInfo));
        memcpy(peerInfo.peer_addr, peerMac, 6);
        peerInfo.channel = 1;
        peerInfo.encrypt = false;
        peerInfo.ifidx = WIFI_IF_STA;
        esp_now_add_peer(&peerInfo);
    }
    return 1;
}