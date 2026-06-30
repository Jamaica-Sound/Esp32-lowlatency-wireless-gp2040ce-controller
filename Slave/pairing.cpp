#include "pairing.h"
#include "wifi_scan.h"
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <Preferences.h>

uint8_t peerMac[6] = {0};

typedef struct __attribute__((packed)) {
    char tag[9];
} PairingPacket;

static bool paired = false;
static bool pendingAck = false;
static uint8_t pendingMac[6] = {0};
static Preferences preferences;

static unsigned long lastAction = 0;
static unsigned long phaseStart = 0;
static enum {
    STATE_IDLE,
    STATE_DISC,
    STATE_WAIT_ACK
} pairingState = STATE_IDLE;

static const unsigned long RETRY_MS = 200;
static const unsigned long TIMEOUT_MS = 5000;
static uint8_t broadcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static void addPeer(uint8_t *mac) {
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

static void savePeerMac(uint8_t *mac) {
    preferences.begin("espnow", false);
    preferences.putBytes("peer_mac", mac, 6);
    preferences.end();
}

static void applyManualChannel() {
    if (manualChannel <= 0) {
        return;
    }

    esp_wifi_set_channel(manualChannel, WIFI_SECOND_CHAN_NONE);
    Serial.printf("[SLAVE] Manual channel set to %d\n", manualChannel);

    bool isZero = true;
    bool isBroadcast = true;
    for (int i = 0; i < 6; i++) {
        if (peerMac[i] != 0)   isZero = false;
        if (peerMac[i] != 0xFF) isBroadcast = false;
    }

    if (!isZero && !isBroadcast) {
        esp_now_del_peer(peerMac);
        addPeer(peerMac);
        Serial.printf("[SLAVE] Peer updated to channel %d\n", manualChannel);
    }
}

static bool loadPeerMac(uint8_t *mac) {
    preferences.begin("espnow", true);
    if (preferences.getBytesLength("peer_mac") == 6) {
        preferences.getBytes("peer_mac", mac, 6);
        preferences.end();
        return true;
    }
    preferences.end();
    return false;
}

static void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    if (len != sizeof(PairingPacket)) {
        wifiScanRecv(info, data, len);
        return;
    }

    PairingPacket *pkt = (PairingPacket*)data;
    Serial.print("[SLAVE] Received packet: ");
    Serial.println(pkt->tag);

    if (paired) return;

    if (!paired && pairingState == STATE_DISC && strcmp(pkt->tag, "JSP_DISC") == 0) {
        Serial.println("[SLAVE] JSP_DISC received");
        memcpy(peerMac, info->src_addr, 6);
        addPeer(peerMac);
        PairingPacket resp;
        strlcpy(resp.tag, "JSP_RESP", sizeof(resp.tag));
        esp_now_send(peerMac, (uint8_t*)&resp, sizeof(resp));
        Serial.println("[SLAVE] JSP_RESP sent");
        pairingState = STATE_WAIT_ACK;
        phaseStart = millis();
        lastAction = 0;
    }
    else if (!paired && pairingState == STATE_WAIT_ACK && strcmp(pkt->tag, "JSP_CONF") == 0) {
        Serial.println("[SLAVE] JSP_CONF received");
        pendingAck = true;
        memcpy(pendingMac, peerMac, 6);
        Serial.println("[SLAVE] ACK queued");
    }
}

void pairingBegin() {

    WiFi.mode(WIFI_STA);
    delay(100);
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    delay(10);
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return;
    }
    esp_now_register_recv_cb(OnDataRecv);
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
            Serial.println("[SLAVE] Broadcast MAC, skip addPeer");
        }

        paired = true;
        pairingState = STATE_IDLE;
        applyManualChannel();
        Serial.println("[SLAVE] Manual MAC configured, pairing bypassed");
        return;
    }

    if (loadPeerMac(peerMac)) {
        addPeer(peerMac);
        paired = true;
        pairingState = STATE_IDLE;
        applyManualChannel();
        Serial.println("[SLAVE] Existing peer restored");
    } else {
        pairingState = STATE_DISC;
        lastAction = 0;
        Serial.println("[SLAVE] Starting discovery");
    }
}

void pairingLoop() {
    if (paired) return;
    unsigned long now = millis();

    if (pendingAck) {
        PairingPacket ack;
        strlcpy(ack.tag, "JSP_ACK", sizeof(ack.tag));
        esp_now_send(pendingMac,(uint8_t*)&ack,sizeof(ack));
        pendingAck = false;
        Serial.println("[SLAVE] ACK sent from loop");
        addPeer(peerMac);
        savePeerMac(peerMac);
        paired = true;
        pairingState = STATE_IDLE;
        applyManualChannel();
        Serial.println("[SLAVE] Pairing completed");
        return;
    }

    if (pairingState == STATE_DISC) {
        if (now - lastAction >= RETRY_MS) {
            PairingPacket disc;
            strlcpy(disc.tag, "JSP_DISC", sizeof(disc.tag));
            esp_now_send(broadcast, (uint8_t*)&disc, sizeof(disc));
            lastAction = now;
            Serial.println("[SLAVE] Sending JSP_DISC");
        }
    } else if (pairingState == STATE_WAIT_ACK) {
        if (now - phaseStart >= TIMEOUT_MS) {
            pairingState = STATE_DISC;
            memset(peerMac, 0, 6);
            Serial.println("[SLAVE] Pairing timeout, restart discovery");
        }
    } 
}

bool pairingReady() {
    return paired;
}

bool isPaired() {
    return paired;
}