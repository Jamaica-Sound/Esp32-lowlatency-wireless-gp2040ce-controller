#include "runtime.h"
#include "pairing.h"
#include "protocol_v2.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_now.h>
#include "soc/gpio_struct.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

#define MAX_PINS 64
#define NVS_NAMESPACE "pin_scanner"
#define KEY_DIGITAL_COUNT "digCount"
#define KEY_ANALOG_COUNT  "anaCount"

Preferences runtimePrefs;

uint8_t digitalPins[64];
uint8_t analogPins[MAX_PINS];

uint8_t digitalCount = 0;
uint8_t analogCount = 0;

uint8_t debounceState[64] = {0};
uint8_t stableDigital[64] = {0};

uint8_t validAnalogCount = 0;
uint8_t validAnalogPins[MAX_PINS];

JSV2_ConfigPacket configPacket;
JSV2_RuntimePacket runtimePacket;

adc_continuous_handle_t adcHandle = NULL;
uint16_t adcLatest[MAX_PINS];
int8_t adcChannelMap[10];

typedef struct {
    uint64_t digitalBits;
    uint16_t analogValues[MAX_PINS];
    uint8_t validAnalogPins[MAX_PINS];
    uint8_t validAnalogCount;
    uint8_t digitalCount;
} runtime_shared_t;

static runtime_shared_t sharedBuffers[2];
static volatile uint8_t currentWriteBuffer = 0;
static volatile uint8_t currentReadBuffer = 1;
static portMUX_TYPE bufferMux = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t txTaskHandle = NULL;
static SemaphoreHandle_t txSemaphore = NULL;
static esp_timer_handle_t pacingTimer = NULL;
static const unsigned long CONFIG_INTERVAL_MS = 1000;

static void txTask(void *pvParameters);
static void pacingCallback(void *arg);

adc_channel_t gpioToAdcChannel(uint8_t pin) {
    switch (pin) {
        case 1:  return ADC_CHANNEL_0;
        case 2:  return ADC_CHANNEL_1;
        case 3:  return ADC_CHANNEL_2;
        case 4:  return ADC_CHANNEL_3;
        case 5:  return ADC_CHANNEL_4;
        case 6:  return ADC_CHANNEL_5;
        case 7:  return ADC_CHANNEL_6;
        case 8:  return ADC_CHANNEL_7;
        case 9:  return ADC_CHANNEL_8;
        case 10: return ADC_CHANNEL_9;
        default: return (adc_channel_t)-1;
    }
}

void loadRuntimeLists() {
    runtimePrefs.begin(NVS_NAMESPACE, true);
    digitalCount = runtimePrefs.getUChar(KEY_DIGITAL_COUNT, 0);
    analogCount  = runtimePrefs.getUChar(KEY_ANALOG_COUNT, 0);
    runtimePrefs.getBytes("digList", digitalPins, digitalCount);
    runtimePrefs.getBytes("anaList", analogPins, analogCount);
    runtimePrefs.end();
}

bool runtimeInit() {
    loadRuntimeLists();

    for (int i = 0; i < digitalCount; i++) {
        pinMode(digitalPins[i], INPUT_PULLUP);
        debounceState[i] = 0;
        stableDigital[i] = 0;
    }

    memset(&sharedBuffers[0], 0, sizeof(runtime_shared_t));
    memset(&sharedBuffers[1], 0, sizeof(runtime_shared_t));
    sharedBuffers[0].digitalCount = digitalCount;
    sharedBuffers[0].validAnalogCount = 0;
    sharedBuffers[1].digitalCount = digitalCount;
    sharedBuffers[1].validAnalogCount = 0;

    adc_digi_pattern_config_t patterns[6] = {};
    memset(adcChannelMap, -1, sizeof(adcChannelMap));
    int validPatterns = 0;
    for (int i = 0; i < analogCount; i++) {
        adc_channel_t ch = gpioToAdcChannel(analogPins[i]);
        if ((int)ch < 0) continue;
        patterns[validPatterns].atten = ADC_ATTEN_DB_11;
        patterns[validPatterns].channel = ch;
        patterns[validPatterns].unit = ADC_UNIT_1;
        patterns[validPatterns].bit_width = ADC_BITWIDTH_12;
        adcChannelMap[ch] = validPatterns;
        validAnalogPins[validPatterns] = analogPins[i];
        validPatterns++;
    }
    validAnalogCount = validPatterns;

    if (validAnalogCount > 0) {
        adc_continuous_handle_cfg_t adcConfig = {
            .max_store_buf_size = 128,
            .conv_frame_size = 64,
        };
        ESP_ERROR_CHECK(adc_continuous_new_handle(&adcConfig, &adcHandle));

        adc_continuous_config_t digCfg = {};
        digCfg.pattern_num   = validPatterns;
        digCfg.adc_pattern   = patterns;
        digCfg.sample_freq_hz = 2000;
        digCfg.conv_mode      = ADC_CONV_SINGLE_UNIT_1;
        digCfg.format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
        ESP_ERROR_CHECK(adc_continuous_config(adcHandle, &digCfg));

        memset(adcLatest, 0, sizeof(adcLatest));
        ESP_ERROR_CHECK(adc_continuous_start(adcHandle));

        uint8_t dump[128];
        uint32_t dumpSize;
        adc_continuous_read(adcHandle, dump, sizeof(dump), &dumpSize, 0);
    }

    configPacket.sync = JSV2_SYNC;
    configPacket.type    = JSV2_TYPE_CONFIG;
    configPacket.digitalCount = digitalCount;
    configPacket.analogCount = validAnalogCount;
memcpy(
           configPacket.pins,
           digitalPins,
           digitalCount
);

memcpy(
    &configPacket.pins[digitalCount],
    validAnalogPins,
    validAnalogCount
);
    uint16_t configSize =
    jsv2_config_size(
        digitalCount,
        validAnalogCount
    );

    uint16_t crc =
    jsv2_crc16(
        (uint8_t*)&configPacket,
        configSize - 2
    );

memcpy(
    ((uint8_t*)&configPacket) + configSize - 2,
    &crc,
    sizeof(uint16_t)
);

    txSemaphore = xSemaphoreCreateBinary();

    esp_timer_create_args_t timerArgs = {
        .callback = pacingCallback,
        .arg = NULL,
        .name = "pacing_timer"
    };
    esp_timer_create(&timerArgs, &pacingTimer);
    
    uint32_t periodo_us;
    if (manualPacingUs > 0) {
        periodo_us = manualPacingUs;
        Serial.printf("[RUNTIME] Usa pacing manuale: %d µs\n", periodo_us);
    } else {
    Preferences prefs;
    prefs.begin("espnow", true);
    periodo_us = prefs.getUInt("pacing_us", 1250);
    prefs.end();
    Serial.printf("[RUNTIME] Periodo pacing: %d µs\n", periodo_us);
            }
    
    esp_timer_start_periodic(pacingTimer, periodo_us);

    xTaskCreatePinnedToCore(
        txTask,
        "txTask",
        8192,
        NULL,
        configMAX_PRIORITIES - 1,
        &txTaskHandle,
        1
    );

    return true;
}

void runtimeLoop() {
    runtime_shared_t *writeBuf = &sharedBuffers[currentWriteBuffer];

    uint32_t gpioLow  = GPIO.in;
    uint32_t gpioHigh = GPIO.in1.data;
    uint64_t bits = 0;
    for (int i = 0; i < digitalCount; i++) {
        uint8_t pin = digitalPins[i];
        bool level;
        if (pin < 32) {
            level = (gpioLow >> pin) & 1;
        } else {
            level = (gpioHigh >> (pin - 32)) & 1;
        }
        bool pressed = !level;

        debounceState[i] = ((debounceState[i] << 1) | pressed) & 0x0F;
        if (debounceState[i] == 0x0F) {
            stableDigital[i] = 1;
        } else if (debounceState[i] == 0x00) {
            stableDigital[i] = 0;
        }
        if (stableDigital[i]) {
            bits |= (1ULL << i);
        }
    }
    
    writeBuf->digitalBits = bits;
    writeBuf->digitalCount = digitalCount;

    if (validAnalogCount > 0 && adcHandle != NULL) {
        static uint8_t result[128];
        static uint32_t bytesRead = 0;
        esp_err_t ret = adc_continuous_read(adcHandle, result, sizeof(result), &bytesRead, 0);
        if (ret == ESP_OK) {
            for (uint32_t i = 0; i < bytesRead; i += sizeof(adc_digi_output_data_t)) {
                adc_digi_output_data_t* p = (adc_digi_output_data_t*)&result[i];
                uint8_t channel = p->type2.channel;
                uint16_t value  = p->type2.data;
                if (channel < 10) {
                    int idx = adcChannelMap[channel];
                    if (idx >= 0) {
                        adcLatest[idx] = value;
                    }
                }
            }
        }
        for (int i = 0; i < validAnalogCount; i++) {
            writeBuf->analogValues[i] = adcLatest[i] << 4;
        }
        writeBuf->validAnalogCount = validAnalogCount;
        memcpy(writeBuf->validAnalogPins, validAnalogPins, validAnalogCount);
    } else {
        writeBuf->validAnalogCount = 0;
    }

    portENTER_CRITICAL(&bufferMux);
    currentWriteBuffer = (currentWriteBuffer == 0) ? 1 : 0;
    currentReadBuffer = (currentReadBuffer == 0) ? 1 : 0;
    portEXIT_CRITICAL(&bufferMux);
}

static void pacingCallback(void *arg) {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(txSemaphore, &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken) portYIELD_FROM_ISR();
}

static void txTask(void *pvParameters) {
    uint32_t lastConfigMillis = millis();
    unsigned long lastDebugPrint = 0;
    uint32_t packetCounter = 0;

    while (1) {
        if (xSemaphoreTake(txSemaphore, portMAX_DELAY) == pdTRUE) {
            portENTER_CRITICAL(&bufferMux);
            runtime_shared_t *readBuf = &sharedBuffers[currentReadBuffer];
            uint64_t digitalBits = readBuf->digitalBits;
            uint16_t analogVals[MAX_PINS];

            memcpy(
                analogVals,
                readBuf->analogValues,
                sizeof(analogVals)
            );

            uint8_t anaCount = readBuf->validAnalogCount;
            portEXIT_CRITICAL(&bufferMux);

            uint16_t runtimeSize =
    jsv2_runtime_size(anaCount);

uint8_t runtimeBuffer[256];

JSV2_RuntimePacket* hdr =
    (JSV2_RuntimePacket*)runtimeBuffer;

hdr->sync = JSV2_SYNC;
hdr->type    = JSV2_TYPE_RUNTIME;
hdr->digitalBits = digitalBits;

    uint8_t* ptr = runtimeBuffer + sizeof(JSV2_RuntimePacket);

    memcpy(
    ptr,
    analogVals,
    anaCount * sizeof(uint16_t)
    );

    uint16_t crc =
        jsv2_crc16(
            runtimeBuffer,
            runtimeSize - 2
    );

    memcpy(
        runtimeBuffer + runtimeSize - 2,
        &crc,
        sizeof(uint16_t)
    );

    esp_err_t err = esp_now_send(
        peerMac,
        runtimeBuffer,
        runtimeSize
);

packetCounter++;

            unsigned long now = millis();
            if (now - lastConfigMillis >= CONFIG_INTERVAL_MS) {

    uint16_t cfgSize =
        jsv2_config_size(
            digitalCount,
            validAnalogCount
        );

auto cfgErr = esp_now_send(
    peerMac,
    (uint8_t*)&configPacket,
    cfgSize
);

    lastConfigMillis = now;
}

             if (now - lastDebugPrint >= 5000) {
                Serial.print("TX rate: ");
                Serial.print(packetCounter / 5);
                Serial.println(" pkt/s");
                packetCounter = 0;
                lastDebugPrint = now;
            } 
        }
    }
}