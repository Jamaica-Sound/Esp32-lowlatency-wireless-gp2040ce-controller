#include "pairing.h"
#include "wifi_scan.h"
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>

typedef struct __attribute__((packed)) {
    char tag[9];
} PairingPacket;

bool paired = false;

uint8_t peerMac[6] = {0};

Preferences preferences;

unsigned long lastAction = 0;
unsigned long phaseStart = 0;

enum {
    STATE_DISC,
    STATE_WAIT_ACK
} state = STATE_DISC;

const unsigned long RETRY = 200;
const unsigned long TIMEOUT = 5000;

uint8_t broadcast[] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF
};

void addPeer(uint8_t *mac) {
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    uint8_t currentChannel;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&currentChannel, &second);
    peer.channel = currentChannel;
    peer.encrypt = false;
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
}

void applyManualChannel() {
    if (manualChannel <= 0) {
        return;
    }

    esp_wifi_set_channel(manualChannel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[PAIR] Manual channel set to %d\n", manualChannel);

    bool isZero = true;
    bool isBroadcast = true;
    for (int i = 0; i < 6; i++) {
        if (peerMac[i] != 0)   isZero = false;
        if (peerMac[i] != 0xFF) isBroadcast = false;
    }

    if (!isZero && !isBroadcast) {
        esp_now_del_peer(peerMac);
        addPeer(peerMac);
        Serial.printf("[PAIR] Peer updated to channel %d\n", manualChannel);
    }
}

void savePeerMac(uint8_t *mac) {

    preferences.begin("espnow", false);

    preferences.putBytes("peer_mac", mac, 6);

    preferences.end();
}

bool loadPeerMac(uint8_t *mac) {

    preferences.begin("espnow", true);

    if (preferences.getBytesLength("peer_mac") == 6) {

        preferences.getBytes("peer_mac", mac, 6);

        preferences.end();

        return true;
    }

    preferences.end();

    return false;
}

void OnDataRecv(
    const esp_now_recv_info *info,
    const uint8_t *data,
    int len)
{   
    Serial.printf("[DEBUG RX] len=%d, first=0x%08X\n", len, ((uint32_t*)data)[0]);

    if (wifiScanWaitingForBestChannel && len == sizeof(BestChannelPacket)) {
        wifiScanHandleBestChannel(data, len);
        return;
    }
    
    if (channelScanActive && len == sizeof(AckPacket)) {
    channelScanHandleAck(data, len);
    return;
    }
    
    if (len != sizeof(PairingPacket)) {
        return;
    }

    Serial.printf(
    "[PAIR RX] len=%d tag=%s state=%d paired=%d\n",
    len,
    ((PairingPacket*)data)->tag,
    state,
    paired
);

    PairingPacket *pkt = (PairingPacket*)data;

    if (
        !paired &&
        state == STATE_DISC &&
        strcmp(pkt->tag, "JSP_RESP") == 0
    ) {
        memcpy(peerMac, info->src_addr, 6);
        addPeer(peerMac);
        state = STATE_WAIT_ACK;
        phaseStart = millis();
        lastAction = 0;
        Serial.println("[PAIR] JSP_RESP received");
    }
    else if (
        !paired &&
        state == STATE_WAIT_ACK &&
        strcmp(pkt->tag, "JSP_ACK") == 0
    ) {
        addPeer(peerMac);
        savePeerMac(peerMac);
        paired = true;
        applyManualChannel();
        Serial.println("[PAIR] Pairing completed");
    }
}

bool isPaired() {

    return paired;
}

void pairingBegin() {
    Serial.println("\n=== PAIRING ===");

    Serial.println("PAIR 1");

    WiFi.mode(WIFI_STA);

    Serial.println("PAIR 2");

    WiFi.disconnect(true, true);

    Serial.println("PAIR 3");

    delay(100);

    yield();

    Serial.println("PAIR 4");

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    Serial.println("PAIR 5");

    if (esp_now_init() != ESP_OK) {

        Serial.println("ESP-NOW init failed");
        return;
    }

    Serial.println("PAIR 6");

    addPeer(broadcast);

    Serial.println("PAIR 7");

    esp_now_register_recv_cb(OnDataRecv);

    Serial.println("PAIR 8");

    bool isManual = false;
    for (int i = 0; i < 6; i++) {
        if (manualPeerMac[i] != 0) {
            isManual = true;
            break;
        }
    }

    if (isManual) {
        savePeerMac(manualPeerMac);
        memcpy(peerMac, manualPeerMac, 6);

        bool isBroadcast = true;
        for (int i = 0; i < 6; i++) {
            if (manualPeerMac[i] != 0xFF) {
                isBroadcast = false;
                break;
            }
        }

        if (!isBroadcast) {
            addPeer(peerMac);
        } else {
            Serial.println("[PAIR] Broadcast MAC, skip addPeer");
        }

        paired = true;
        Serial.println("[PAIR] Manual MAC configured, pairing bypassed");
        applyManualChannel();
        return;
    }

    if (loadPeerMac(peerMac)) {
        addPeer(peerMac);
        paired = true;
        applyManualChannel();
        Serial.println("[PAIR] Existing peer restored");
    } else {
        state = STATE_DISC;
        lastAction = 0;
        Serial.println("[PAIR] Starting discovery");
    }
}

void pairingLoop()
{
    if (paired) {
        return;
    }

    unsigned long now = millis();

    if (state == STATE_DISC) {

        if (now - lastAction >= RETRY) {

            Serial.println("[PAIR TX] JSP_DISC");

            PairingPacket disc;
            strlcpy(disc.tag, "JSP_DISC", sizeof(disc.tag));

            esp_now_send(
                broadcast,
                (uint8_t*)&disc,
                sizeof(disc)
            );

            lastAction = now;
        }
    }
    else if (state == STATE_WAIT_ACK) {
        if (now - lastAction >= RETRY) {

            Serial.println("[PAIR TX] JSP_CONF");

            PairingPacket conf;

            strlcpy(conf.tag, "JSP_CONF", sizeof(conf.tag));

            esp_now_send(
                peerMac,
                (uint8_t*)&conf,
                sizeof(conf)
            );

            lastAction = now;
        }

        if (now - phaseStart >= TIMEOUT) {

            state = STATE_DISC;

            memset(peerMac, 0, 6);

            Serial.println("[PAIR] Timeout restart");
        }
    }
}

bool pairingReady()
{
    return paired;
}