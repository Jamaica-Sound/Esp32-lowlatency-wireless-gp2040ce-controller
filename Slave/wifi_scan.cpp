#include "pairing.h"
#include "wifi_scan.h"
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>

extern uint8_t peerMac[6];
extern int8_t manualChannel;

#define MAX_CHANNEL 13

uint32_t packetsReceived[MAX_CHANNEL + 1] = {0};
uint32_t packetsReceivedTotal = 0;
uint32_t testEndTime = 0;
uint8_t currentTestChannel = 0;

bool countingActive = false;
bool rankingPrinted = false;
bool testComplete = false;
bool finalRankingPrinted = false;
bool wifiScanComplete = false;
bool wifiScanWaitingForBestChannel = false;
static uint8_t bestChannelToSend = 0;
static bool waitingMessagePrinted = false;
static bool bestChannelConfirmed = false;

void updatePeerChannel(uint8_t *mac, uint8_t channel) {
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
        Serial.printf("Error adding peer on channel %d: %d\n", channel, err);
    }
    delay(20);
}

void printFinalRanking() {
    Serial.println("\n==================================");
    Serial.println("  FINAL RANKING RECEIVED PACKET");
    Serial.println("====================================");
    Serial.println("Channel | Packets received");
    Serial.println("----------------------------------------");

    typedef struct {
        uint8_t channel;
        uint32_t count;
    } RankItem;
    
    RankItem items[MAX_CHANNEL + 1];
    int numItems = 0;
    for (int ch = 1; ch <= MAX_CHANNEL; ch++) {
        items[numItems].channel = ch;
        items[numItems].count = packetsReceived[ch];
        numItems++;
    }

    for (int i = 0; i < numItems - 1; i++) {
        for (int j = 0; j < numItems - i - 1; j++) {
            if (items[j].count < items[j+1].count) {
                RankItem temp = items[j];
                items[j] = items[j+1];
                items[j+1] = temp;
            }
        }
    }

    for (int i = 0; i < numItems; i++) {
        Serial.printf("  %2d   |   %5d\n", items[i].channel, items[i].count);
    }

    Serial.println("----------------------------------------");
    if (numItems > 0) {
        Serial.printf(">>> BEST CHANNEL: %d (%d packets) <<<\n",
                      items[0].channel, items[0].count);
        bestChannelToSend = items[0].channel;
    }
    Serial.println("========================================");
    Serial.printf("Total packets received: %d\n", packetsReceivedTotal);
}

void printChannelResult(uint8_t channel) {
    Serial.printf("\n[Slave] Channel %d completed: %d packets\n",
                  channel, packetsReceived[channel]);
}

void wifiScanRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    if (len == sizeof(BestChannelPacket)) {
        BestChannelPacket *pkt = (BestChannelPacket*)data;
        if (pkt->magic == 0xDEADBEEF && pkt->status == 0) {
            bestChannelConfirmed = true;
            wifiScanWaitingForBestChannel = false;
            Serial.printf("[WIFI-SLAVE] ACK received from master for channel %d\n", pkt->bestChannel);
        }
        return;
    }
    
    if (len == sizeof(SyncPacket)) {
        SyncPacket *sync = (SyncPacket*)data;
        if (sync->magic == 0xDEADBEEF) {
            uint8_t newChannel = sync->newChannel;

            if (newChannel == 0) {
                testComplete = true;
                return;
            }

            for (int i = 0; i < 5; i++) {
                AckPacket ack;
                ack.magic = 0xDEADBEEF;
                ack.channel = newChannel;
                ack.status = 0;
                esp_now_send(peerMac, (uint8_t*)&ack, sizeof(ack));
                delay(10);
            }
            
            esp_wifi_set_channel(newChannel, WIFI_SECOND_CHAN_NONE);
            delay(50);
            updatePeerChannel(peerMac, newChannel);
            delay(50);

            packetsReceived[newChannel] = 0;
            currentTestChannel = newChannel;
            countingActive = true;
            testEndTime = millis() + sync->testDuration;
            rankingPrinted = false;
            return;
        }
    }

    if (countingActive && millis() < testEndTime && currentTestChannel > 0) {
        packetsReceived[currentTestChannel]++;
        packetsReceivedTotal++;
    }
}

void wifiScanHandleAck(const uint8_t *data, int len) {
}

void wifiScanLoop() {
    if (countingActive && millis() >= testEndTime) {
        countingActive = false;
        delay(500);
        Serial.flush();
        if (!rankingPrinted) {
            printChannelResult(currentTestChannel);
            rankingPrinted = true;
        }
    }

    if (testComplete && !finalRankingPrinted) {
        testComplete = false;
        Serial.flush();
        delay(500);
        printFinalRanking();
        finalRankingPrinted = true;

        if (bestChannelToSend > 0) {
            BestChannelPacket bestPkt;
            bestPkt.magic = 0xDEADBEEF;
            bestPkt.bestChannel = bestChannelToSend;
            bestPkt.packetCount = packetsReceived[bestChannelToSend];
            bestPkt.status = 0;
            esp_now_send(peerMac, (uint8_t*)&bestPkt, sizeof(bestPkt));
            Serial.printf("[WIFI-SLAVE] Best channel sent: %d\n", bestChannelToSend);

            wifiScanWaitingForBestChannel = true;
            bestChannelConfirmed = false;
            unsigned long start = millis();
            while (!bestChannelConfirmed && (millis() - start < 5000)) {
                delay(50);
            }

            if (bestChannelConfirmed) {
                Serial.printf("[WIFI-SLAVE] ACK received from master for channel %d\n", bestChannelToSend);
                Preferences prefs;
                prefs.begin("espnow", false);
                prefs.putUChar("best_channel", bestChannelToSend);
                prefs.end();
                Serial.printf("[WIFI-SLAVE] Channel %d saved to NVS\n", bestChannelToSend);
                wifiScanComplete = true;
            } else {
                Serial.println("[WIFI-SLAVE] ACK timeout; retrying...");
                esp_now_send(peerMac, (uint8_t*)&bestPkt, sizeof(bestPkt));
                delay(500);
                wifiScanComplete = true;
            }
        }
    }
}

uint8_t wifiChannelSelectOrApply() {
    if (manualChannel > 0) {
        uint8_t ch = manualChannel;
        Serial.printf("[WIFI-SLAVE] Using manual channel: %d\n", ch);
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
        Serial.println("[WIFI-SLAVE] Using default channel: 1");
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

    Preferences prefs;
    prefs.begin("espnow", true);
    uint8_t savedChannel = prefs.getUChar("best_channel", 0);
    prefs.end();

    if (savedChannel >= 1 && savedChannel <= 13) {
        Serial.printf("[WIFI-SLAVE] Saved channel found: %d\n", savedChannel);
        esp_wifi_set_channel(savedChannel, WIFI_SECOND_CHAN_NONE);
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
            peerInfo.channel = savedChannel;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return savedChannel;
    }

    if (wifiScanComplete && bestChannelToSend > 0) {
        esp_wifi_set_channel(bestChannelToSend, WIFI_SECOND_CHAN_NONE);
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
            peerInfo.channel = bestChannelToSend;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return bestChannelToSend;
    }

    if (!waitingMessagePrinted) {
        Serial.println("[WIFI-SLAVE] Waiting for the scan from the master...");
        waitingMessagePrinted = true;
    }
    
    while (!wifiScanComplete) {
    return 0;
    }

    if (wifiScanComplete && bestChannelToSend > 0) {
        Serial.printf("[WIFI-SLAVE] Channel received from master: %d\n", bestChannelToSend);
        esp_wifi_set_channel(bestChannelToSend, WIFI_SECOND_CHAN_NONE);
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
            peerInfo.channel = bestChannelToSend;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
        }
        return bestChannelToSend;
    }

    Serial.println("[WIFI-SLAVE] Timeout or failure; using channel 1");
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