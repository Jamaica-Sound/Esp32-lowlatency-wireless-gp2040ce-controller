#include <Arduino.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include "runtime.h"
#include "pairing.h"
#include "protocol_v2.h"
#include "pairing.h"
#include "WiFi.h"
#include <esp_now.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

TaskHandle_t uartTaskHandle = nullptr;

volatile bool runtimePacketReady = false;
volatile bool configPacketReady = false;
volatile uint32_t runtimeCount = 0;
volatile uint32_t uartTxCount = 0;
volatile int minFreeTx = 999999;
volatile uint32_t configTxCount = 0;

uint8_t runtimePacketBuffer[256];
uint16_t runtimePacketSize = 0;
uint8_t configPacketBuffer[256];
uint16_t configPacketSize = 0;

Preferences uartPrefs;

int uartRxPin = -1;
int uartTxPin = -1;
uint32_t uartBaud = 0;

portMUX_TYPE runtimeMux = portMUX_INITIALIZER_UNLOCKED;

HardwareSerial& picoUart = Serial2;

void setupRuntimeUart(
    int rxPin,
    int txPin,
    uint32_t baud
)
{
    picoUart.end();

    picoUart.setRxBufferSize(2048);
    picoUart.setTxBufferSize(2048);

    picoUart.begin(
        baud,
        SERIAL_8N1,
        rxPin,
        txPin
    );

    uart_port_t uart = UART_NUM_2;

    uart_set_baudrate(
        uart,
        baud
    );

    uart_set_word_length(
        uart,
        UART_DATA_8_BITS
    );

    uart_set_parity(
        uart,
        UART_PARITY_DISABLE
    );

    uart_set_stop_bits(
        uart,
        UART_STOP_BITS_1
    );

    uart_set_hw_flow_ctrl(
        uart,
        UART_HW_FLOWCTRL_DISABLE,
        0
    );

    uart_set_rx_timeout(
        uart,
        1
    );

    uart_set_always_rx_timeout(
        uart,
        true
    );
}

void runtimeEspNowRecv(
    const esp_now_recv_info *info,
    const uint8_t *data,
    int len
) {

    if (len < 5) {
        return;
    }

    if (len > sizeof(runtimePacketBuffer)) {
    return;
    }
    

  uint16_t sync;

    memcpy(
      &sync,
      data,
      sizeof(uint16_t)
    );

    if (sync != JSV2_SYNC) {
      return;
    }

    const JSV2_RuntimePacket* hdr = (const JSV2_RuntimePacket*)data;
    uint8_t type = hdr->type;
    portENTER_CRITICAL_ISR(&runtimeMux);

    if (type == JSV2_TYPE_RUNTIME) {
        runtimePacketReady = true;
        memcpy(
            runtimePacketBuffer,
            data,
            len
        );
        runtimePacketSize = len;
    }

else if (type == JSV2_TYPE_CONFIG) {

    memcpy(
        configPacketBuffer,
        data,
        len
    );

    configPacketSize = len;
    configPacketReady = true;
}

    portEXIT_CRITICAL_ISR(&runtimeMux);

    if (uartTaskHandle != nullptr) {
        xTaskNotifyGive(uartTaskHandle);
    }
}

void uartRuntimeTask(void *p) {
    uint8_t localRuntime[256];
    uint16_t localRuntimeSize = 0;
    uint8_t localConfig[256];
    uint16_t localConfigSize = 0;
    bool sendRuntime = false;
    bool sendConfig = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (1) {        
        portENTER_CRITICAL(&runtimeMux);
        if (runtimePacketReady) {
            memcpy(localRuntime, runtimePacketBuffer, runtimePacketSize);
            localRuntimeSize = runtimePacketSize;
            runtimePacketReady = false;
            sendRuntime = true;
        }
        if (configPacketReady) {
            memcpy(localConfig, configPacketBuffer, configPacketSize);
            localConfigSize = configPacketSize;
            configPacketReady = false;
            sendConfig = true;
        }
        portEXIT_CRITICAL(&runtimeMux);

        if (!sendRuntime && !sendConfig) {
            break;
        }

        if (sendRuntime) {

    int freeBefore =
        picoUart.availableForWrite();

    if (freeBefore < minFreeTx)
        minFreeTx = freeBefore;

    if (freeBefore < localRuntimeSize) {

        Serial.printf(
            "[UART FULL] free=%d need=%u\n",
            freeBefore,
            localRuntimeSize
        );
    }

    size_t written =
        picoUart.write(
            localRuntime,
            localRuntimeSize
        );

    if (written != localRuntimeSize) {
        Serial.printf(
            "[UART DROP] wanted=%u written=%u\n",
            localRuntimeSize,
            (unsigned)written
        );
    }
    uartTxCount++;
    sendRuntime = false;
}

        if (sendConfig) {
            picoUart.write(localConfig, localConfigSize);
            configTxCount++;
            sendConfig = false;
           }
        }
    }    
}

bool runtimeInit() {
    
    Serial.println("DEBUG: runtimeInit entered");
    Serial.flush();
    delay(10);
    
    Serial.println("DEBUG: uartPrefs.begin");
    uartPrefs.begin("uart_config", true);
    Serial.println("DEBUG: after uartPrefs.begin");
    
    uartRxPin = uartPrefs.getInt("rx", -1);
    uartTxPin = uartPrefs.getInt("tx", -1);
    uartBaud = uartPrefs.getUInt("baud", 9600);
    Serial.printf("DEBUG: pins %d %d baud %lu\n", uartRxPin, uartTxPin, uartBaud);
    
    uartPrefs.end();
    Serial.println("DEBUG: uartPrefs.end");

    if (
        uartRxPin < 0 ||
        uartTxPin < 0
    ) {
        return false;
    }

    Serial.println("DEBUG: calling setupRuntimeUart");
    setupRuntimeUart(uartRxPin, uartTxPin, uartBaud);
    Serial.println("DEBUG: setupRuntimeUart returned");
    
    esp_now_register_recv_cb(runtimeEspNowRecv);
    Serial.println("ESP-NOW callback registered");

 Serial.println("[RUNTIME] reading peer from NVS");
    delay(10);
    Preferences pairingPrefs;
    pairingPrefs.begin("espnow", true);
    uint8_t mac[6] = {0};
    if (pairingPrefs.getBytesLength("peer_mac") == 6) {
        pairingPrefs.getBytes("peer_mac", mac, 6);
        Serial.println("[RUNTIME] peer MAC loaded from NVS");
        delay(10);
        esp_now_peer_info_t peerInfo;
        if (esp_now_get_peer(mac, &peerInfo) != ESP_OK) {
            Serial.println("[RUNTIME] peer not found, adding...");
            memset(&peerInfo, 0, sizeof(peerInfo));
            memcpy(peerInfo.peer_addr, mac, 6);
            peerInfo.channel = 0;
            peerInfo.encrypt = false;
            peerInfo.ifidx = WIFI_IF_STA;
            esp_now_add_peer(&peerInfo);
            Serial.println("Peer re-added in runtime");
        } else {
            Serial.println("Peer already active");
        }
    }
    pairingPrefs.end();

    Serial.printf("WiFi channel after pairing: %d\n", WiFi.channel());
    Serial.print("Peer MAC: ");
    for (int i=0; i<6; i++) {
        Serial.printf("%02X%s", mac[i], i<5 ? ":" : "");
    }
    Serial.println();

    return true;
}

void runtimeStart() {

    xTaskCreatePinnedToCore(
        uartRuntimeTask,
        "uartRuntime",
        8192,
        NULL,
        1,
        &uartTaskHandle,
        1
    );
}

void runtimeLoop() {

    static uint32_t lastRuntimeRx = 0;
    static uint32_t lastRuntimeTx = 0;
    static uint32_t lastConfigTx = 0;
    static uint32_t lastPrint = 0;

    uint32_t now = millis();

    if (now - lastPrint >= 5000) {

        uint32_t rxNow = runtimeCount;
        uint32_t runtimeTxNow = uartTxCount;
        uint32_t configTxNow = configTxCount;

        Serial.printf(
        "[STATS 5s] ESPNOW CRC OK=%lu UART_RUNTIME_TX=%lu UART_CONFIG_TX=%lu MINFREE=%d\n",
    (unsigned long)(rxNow - lastRuntimeRx),
    (unsigned long)(runtimeTxNow - lastRuntimeTx),
    (unsigned long)(configTxNow - lastConfigTx),
    minFreeTx
        );

        lastRuntimeRx = rxNow;
        lastRuntimeTx = runtimeTxNow;
        lastConfigTx = configTxNow;
        lastPrint = now;
    }
}