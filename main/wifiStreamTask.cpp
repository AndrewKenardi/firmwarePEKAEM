/*
 * wifiStreamTask.cpp
 *
 * FIX: ml_safe_fb_return() sekarang mengambil camera_capture_mutex
 * (didefinisikan di cameraTask.c) sebelum memanggil esp_camera_fb_return().
 * Tanpa ini, esp_camera_fb_get() (dipanggil dari vTaskCameraRead di core lain)
 * dan esp_camera_fb_return() (dipanggil dari vTaskMLStream di sini) bisa
 * berjalan bersamaan dan merusak state internal pool frame buffer driver
 * esp32-camera, yang berujung pada timeout terus-menerus saat capture.
 */

#include "wifiStreamTask.h"
#include "cameraTask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
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

// FIX: camera_capture_mutex sudah dideklarasikan sebagai extern di
// cameraTask.h (di-include di atas) dan didefinisikan di cameraTask.c.
// Mutex ini WAJIB dipakai setiap kali esp_camera_fb_get()/fb_return()
// dipanggil dari task manapun, agar tidak ada dua task yang menyentuh
// pool frame buffer driver kamera secara bersamaan. Tidak perlu
// dideklarasikan ulang di sini — cukup pakai langsung.

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

// Batas maksimum alokasi buffer pengiriman paket (100 KB cukup untuk frame JPEG)
#define MAX_TX_PACKET_SIZE (100 * 1024)

// Delay antar pengiriman frame (35 ms ≈ 25-28 FPS).
// Menaikkan angka ini ke 40-50 ms akan membuat koneksi jauh lebih stabil jika Wi-Fi lambat.
#define STREAM_FRAME_DELAY_MS 35

// Timeout maksimum menunggu mutex kamera saat akan me-return frame buffer.
// Diberi batas (bukan portMAX_DELAY) supaya task streaming tidak ikut macet
// selamanya kalau task lain menahan mutex lebih lama dari yang diharapkan.
#define CAMERA_MUTEX_WAIT_MS 200

static WebSocketsClient s_ws;
static bool s_ws_started = false;
static bool s_ws_fully_connected = false; // Flag status SSL Handshake
static bool s_arduino_inited = false;

// Mutex & Variabel Jarak Sensor VL53L0X
static SemaphoreHandle_t s_distance_mutex = NULL;
static uint16_t s_last_distance_mm = 0;
static bool s_last_distance_valid = false;

// Buffer reuse statis untuk transmisi data
static uint8_t *s_tx_packet_buffer = NULL;

// ============================================================================
// HELPER: Kembalikan frame buffer kamera dengan aman
// ============================================================================
// FIX: Sekarang mengunci camera_capture_mutex sebelum memanggil
// esp_camera_fb_return(), supaya tidak bentrok dengan esp_camera_fb_get()
// yang dipanggil dari vTaskCameraRead (core/task berbeda).
static inline void ml_safe_fb_return(camera_fb_t *fb) {
    if (fb == NULL) return;

    if (camera_capture_mutex != NULL) {
        if (xSemaphoreTake(camera_capture_mutex, pdMS_TO_TICKS(CAMERA_MUTEX_WAIT_MS)) == pdTRUE) {
            esp_camera_fb_return(fb);
            xSemaphoreGive(camera_capture_mutex);
        } else {
            // Gagal ambil mutex dalam batas waktu — tetap return buffer
            // supaya tidak leak, tapi catat sebagai peringatan karena ini
            // mengindikasikan kontensi tinggi antara kedua task.
            ESP_LOGW(TAG_WS, "Timeout menunggu camera_capture_mutex saat fb_return, tetap dipaksa return");
            esp_camera_fb_return(fb);
        }
    } else {
        // Mutex belum diinisialisasi (mis. dipanggil sebelum init_camera_driver()).
        // Return tetap dilakukan agar tidak leak, tapi tanpa proteksi.
        esp_camera_fb_return(fb);
    }
}

extern "C" void ml_stream_init(void) {
    if (s_distance_mutex == NULL) {
        s_distance_mutex = xSemaphoreCreateMutex();
    }

    // Alokasikan buffer pengiriman sekali saja di PSRAM jika ada, atau DRAM
    if (s_tx_packet_buffer == NULL) {
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32_SPIRAM_SUPPORT
        s_tx_packet_buffer = (uint8_t *) heap_caps_malloc(MAX_TX_PACKET_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_tx_packet_buffer) {
            ESP_LOGI(TAG_WS, "Buffer pengiriman WebSocket dialokasikan di PSRAM (%d bytes)", MAX_TX_PACKET_SIZE);
        }
#endif
        if (s_tx_packet_buffer == NULL) {
            s_tx_packet_buffer = (uint8_t *) malloc(MAX_TX_PACKET_SIZE);
            if (s_tx_packet_buffer) {
                ESP_LOGI(TAG_WS, "Buffer pengiriman WebSocket dialokasikan di Internal DRAM (%d bytes)", MAX_TX_PACKET_SIZE);
            } else {
                ESP_LOGE(TAG_WS, "FATAL: Gagal mengalokasikan memori buffer pengiriman WebSocket!");
            }
        }
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
            s_ws_fully_connected = false;
            ESP_LOGW(TAG_WS, "[WS] Terputus dari ML Server.");
            break;

        case WStype_CONNECTED:
            s_ws_fully_connected = true;
            ESP_LOGI(TAG_WS, "[WS] BERHASIL TERHUBUNG DENGAN SSL! Host: %s", WS_SERVER_HOST);
            break;

        case WStype_TEXT:
            ESP_LOGI(TAG_WS, "[WS] Server balas: %.*s", (int)length, payload);
            break;

        case WStype_ERROR:
            s_ws_fully_connected = false;
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
        // Proses polling internal library WebSocket (Ping/Pong, Reconnect, RX)
        s_ws.loop();

        // 1. JIKA WEBSOCKET BELUM KONEK: DRAIN QUEUE & BERI JEDA UNTUK SSL HANDSHAKE
        if (!s_ws_fully_connected) {
            camera_fb_t *dummy_fb = NULL;
            while (xQueueReceive(frame_queue, &dummy_fb, 0) == pdTRUE) {
                ml_safe_fb_return(dummy_fb);
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // Jeda agar stack TLS/Network punya waktu re-handshake
            continue;
        }

        // 2. AMBIL FRAME TERBARU DARI QUEUE
        camera_fb_t *fb = NULL;
        if (xQueueReceive(frame_queue, &fb, pdMS_TO_TICKS(10)) == pdTRUE && fb != NULL) {

            size_t required_packet_size = 1 + robot_id_len + 1 + fb->len;

            if (s_tx_packet_buffer != NULL && required_packet_size <= MAX_TX_PACKET_SIZE) {

                uint16_t distance_mm = 0;
                bool distance_valid = false;
                ml_get_distance(&distance_mm, &distance_valid);
                uint8_t is_dekat = (distance_valid && distance_mm <= EYE_DISTANCE_THRESHOLD_MM) ? 1 : 0;

                // Susun Payload Biner: [Len ID][ID Robot][Is Dekat][Data JPEG]
                s_tx_packet_buffer[0] = robot_id_len;
                memcpy(s_tx_packet_buffer + 1, robot_id, robot_id_len);
                s_tx_packet_buffer[1 + robot_id_len] = is_dekat;
                memcpy(s_tx_packet_buffer + 1 + robot_id_len + 1, fb->buf, fb->len);

                // Kirim frame biner
                bool sent = s_ws.sendBIN(s_tx_packet_buffer, required_packet_size);
                if (!sent) {
                    ESP_LOGE(TAG_WS, "Gagal mengirim data biner WebSocket (Socket Penuh / Timeout)");
                } else {
                    // Throttling: Beri waktu jeda pengiriman agar TCP/SSL socket tidak overload
                    vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_DELAY_MS));
                }
            } else {
                ESP_LOGE(TAG_WS, "Ukuran paket (%d bytes) melebihi batas buffer!", required_packet_size);
            }

            // Kembalikan Frame Buffer SEGERA ke Driver Kamera (sudah dilindungi mutex)
            ml_safe_fb_return(fb);
            fb = NULL;
        } else {
            // Jika queue kosong, pasrahkan CPU sebentar ke task lain
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

extern "C" void vTaskWifiConnect(void *pvParameter) {
    while (connect_wifi() != ESP_OK) {
        ESP_LOGW("WIFI", "Wi-Fi belum terhubung, mencoba ulang...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("WIFI", "Wi-Fi Berhasil Terhubung!");
    xEventGroupSetBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT);

    // Matikan mode hemat daya Wi-Fi untuk stabilitas streaming latency rendah
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelete(NULL);
}