/*
 * wifiStreamTask.cpp
 */

#include "wifiStreamTask.h"
#include "cameraTask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

// --- Arduino core & library WebSocket (C++) ---
#include <Arduino.h>
#include <WebSocketsClient_Generic.h>

extern EventGroupHandle_t wifiEventGroup;
extern QueueHandle_t frame_queue;
extern "C" esp_err_t connect_wifi(void);

#ifndef IS_WIFI_CONNECTED_BIT
#define IS_WIFI_CONNECTED_BIT BIT0
#endif

static const char *TAG_WS = "WS_STREAM";

// ============================================================================
// KONFIGURASI SERVER WEBSOCKET (LIVE PRODUCTION)
// ============================================================================
#define WS_SERVER_HOST "socasob-ml.hallojanu.xyz"
#define WS_SERVER_PORT 443
#define WS_SERVER_PATH "/ws"
#define EYE_DISTANCE_THRESHOLD_MM 300 // Jarak <= 30cm dianggap "Dekat"

static WebSocketsClient s_ws;
static bool s_ws_started = false;
static bool s_arduino_inited = false;

// Mutex & Variabel Jarak Sensor VL53L0X
static SemaphoreHandle_t s_distance_mutex = NULL;
static uint16_t s_last_distance_mm = 0;
static bool s_last_distance_valid = false;

// Buffer reuse statis untuk transmisi data
static uint8_t *s_tx_packet_buffer = NULL;
static size_t s_tx_packet_buffer_size = 0;

// ============================================================================
// HELPER: Kembalikan frame buffer kamera dengan lock camera_capture_mutex
// ============================================================================
static inline void ml_safe_fb_return(camera_fb_t *fb) {
    if (fb == NULL) return;
    if (camera_capture_mutex != NULL) {
        xSemaphoreTake(camera_capture_mutex, portMAX_DELAY);
    }
    esp_camera_fb_return(fb);
    if (camera_capture_mutex != NULL) {
        xSemaphoreGive(camera_capture_mutex);
    }
}

extern "C" void ml_stream_init(void) {
    if (s_distance_mutex == NULL) {
        s_distance_mutex = xSemaphoreCreateMutex();
    }
}

extern "C" void ml_stream_set_distance(uint16_t distance_mm, bool valid) {
    if (s_distance_mutex == NULL) {
        ml_stream_init();
    }
    if (s_distance_mutex != NULL && xSemaphoreTake(s_distance_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_last_distance_mm = distance_mm;
        s_last_distance_valid = valid;
        xSemaphoreGive(s_distance_mutex);
    }
}

static void ml_get_distance(uint16_t *distance_mm, bool *valid) {
    *distance_mm = 0;
    *valid = false;
    if (s_distance_mutex == NULL) return;
    if (xSemaphoreTake(s_distance_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *distance_mm = s_last_distance_mm;
        *valid = s_last_distance_valid;
        xSemaphoreGive(s_distance_mutex);
    }
}

// ============================================================================
// WEBSOCKET EVENT HANDLER
// ============================================================================
static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            ESP_LOGW(TAG_WS, "[WS] Terputus dari ML Server.");
            break;
        case WStype_CONNECTED:
            ESP_LOGI(TAG_WS, "[WS] BERHASIL TERHUBUNG! Host: %s", WS_SERVER_HOST);
            break;
        case WStype_TEXT:
            ESP_LOGI(TAG_WS, "[WS] Server balas: %.*s", (int)length, payload);
            break;
        case WStype_ERROR:
            ESP_LOGE(TAG_WS, "[WS] Error event WebSocket");
            break;
        default:
            break;
    }
}

static void ml_init_websocket(void) {
    if (s_ws_started) {
        return;
    }

    s_ws.beginSSL(WS_SERVER_HOST, WS_SERVER_PORT, WS_SERVER_PATH);
    s_ws.onEvent(webSocketEvent);
    s_ws.setReconnectInterval(3000);

    s_ws_started = true;
}

// ============================================================================
// TASK UTAMA STREAMING FRAME WEBSOCKET
// ============================================================================
extern "C" void vTaskMLStream(void *pvParameters) {
    ESP_LOGI(TAG_WS, "=== TASK ML STREAM DIPANGGIL DAN MULAI BERJALAN ===");
    
    // PASTIKAN robot_id ini sudah terdaftar di database Backend
    const char *robot_id = "dummyrobot01";
    uint8_t robot_id_len = (uint8_t)strlen(robot_id);

    ml_stream_init();

    // Tunggu Wi-Fi terhubung
    xEventGroupWaitBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (!s_arduino_inited) {
        initArduino();
        s_arduino_inited = true;
    }

    ESP_LOGI(TAG_WS, "Wi-Fi Terverifikasi Konek, Menginisialisasi Client WebSocket...");
    ml_init_websocket();

    for (;;) {
        s_ws.loop();

        bool ws_connected = s_ws.isConnected();

        // JIKA WEBSOCKET BELUM KONEK: DRAIN QUEUE SECARA CEPAT
        if (!ws_connected) {
            camera_fb_t *dummy_fb = NULL;
            while (xQueueReceive(frame_queue, &dummy_fb, 0) == pdTRUE) {
                ml_safe_fb_return(dummy_fb);
            }
            vTaskDelay(pdMS_TO_TICKS(20)); // Delay pendek agar buffer kamera tidak numpuk
            continue;
        }

        // AMBIL FRAME TERBARU DARI QUEUE
        camera_fb_t *fb = NULL;
        if (xQueueReceive(frame_queue, &fb, pdMS_TO_TICKS(20)) == pdTRUE && fb != NULL) {

            uint16_t distance_mm = 0;
            bool distance_valid = false;
            ml_get_distance(&distance_mm, &distance_valid);
            uint8_t is_dekat = (distance_valid && distance_mm <= EYE_DISTANCE_THRESHOLD_MM) ? 1 : 0;

            size_t required_packet_size = 1 + robot_id_len + 1 + fb->len;
            if (s_tx_packet_buffer == NULL || s_tx_packet_buffer_size < required_packet_size) {
                uint8_t *new_buf = (uint8_t *)realloc(s_tx_packet_buffer, required_packet_size);
                if (new_buf == NULL) {
                    ESP_LOGE(TAG_WS, "Gagal mengalokasikan realloc paket buffer!");
                    ml_safe_fb_return(fb);
                    taskYIELD();
                    continue;
                }
                s_tx_packet_buffer = new_buf;
                s_tx_packet_buffer_size = required_packet_size;
            }

            // Susun Payload Biner
            s_tx_packet_buffer[0] = robot_id_len;
            memcpy(s_tx_packet_buffer + 1, robot_id, robot_id_len);
            s_tx_packet_buffer[1 + robot_id_len] = is_dekat;
            memcpy(s_tx_packet_buffer + 1 + robot_id_len + 1, fb->buf, fb->len);

            // Kirim frame biner
            bool sent = s_ws.sendBIN(s_tx_packet_buffer, required_packet_size);
            if (!sent) {
                ESP_LOGE(TAG_WS, "Gagal mengirim data biner WebSocket");
            }

            // Kembalikan Frame Buffer SEGERA ke Driver Kamera
            ml_safe_fb_return(fb);
            fb = NULL;
        }

        taskYIELD(); // Beri giliran CPU ke task kamera & wifi stack
    }
}

extern "C" void vTaskWifiConnect(void *pvParameter) {
    while (connect_wifi() != ESP_OK) {
        ESP_LOGW("WIFI", "Wi-Fi belum terhubung, mencoba ulang...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("WIFI", "Wi-Fi Berhasil Terhubung!");
    xEventGroupSetBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT);

    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelete(NULL);
}