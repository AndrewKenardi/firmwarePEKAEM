#include "wifiStreamTask.h"
#include "cameraTask.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

extern EventGroupHandle_t wifiEventGroup;
extern QueueHandle_t frame_queue;
extern esp_err_t connect_wifi(void);

#ifndef IS_WIFI_CONNECTED_BIT
#define IS_WIFI_CONNECTED_BIT BIT0
#endif

static const char *TAG_WS = "WS_STREAM";

// ============================================================================
// KONFIGURASI SERVER WEBSOCKET
// ============================================================================
#define WS_SERVER_URI "ws://192.168.100.28:5000/ws"
#define EYE_DISTANCE_THRESHOLD_MM 300 // Jarak <= 30cm dianggap "Dekat"

static esp_websocket_client_handle_t s_ws_client = NULL;

// Mutex & Variabel Jarak Sensor VL53L0X
static SemaphoreHandle_t s_distance_mutex = NULL;
static uint16_t s_last_distance_mm = 0;
static bool s_last_distance_valid = false;

// Buffer reuse statis untuk transmisi data (mencegah fragmentasi heap RAM)
static uint8_t *s_tx_packet_buffer = NULL;
static size_t s_tx_packet_buffer_size = 0;

// ============================================================================
// HELPER FUNGSIONALITAS MUTEX JARAK
// ============================================================================
void ml_stream_init(void) {
    if (s_distance_mutex == NULL) {
        s_distance_mutex = xSemaphoreCreateMutex();
    }
}

void ml_stream_set_distance(uint16_t distance_mm, bool valid) {
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
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG_WS, "Terhubung ke WebSocket ML Server!");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG_WS, "Koneksi WebSocket terputus!");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                ESP_LOGI(TAG_WS, "Pesan Server: %.*s", data->data_len, data->data_ptr);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG_WS, "WebSocket Error!");
            break;
    }
}

static void ml_init_websocket(void) {
    if (s_ws_client != NULL) {
        return; // Cegah double initialization
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = WS_SERVER_URI,
        .buffer_size = 2048,
        .skip_cert_common_name_check = true,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (s_ws_client == NULL) {
        ESP_LOGE(TAG_WS, "Gagal mengalokasikan memori client WebSocket!");
        return;
    }

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_websocket_client_start(s_ws_client);
}

// ============================================================================
// TASK UTAMA STREAMING FRAME WEBSOCKET
// ============================================================================
void vTaskMLStream(void *pvParameters) {
    ESP_LOGI(TAG_WS, "=== TASK ML STREAM DIPANGGUL DAN MULAI BERJALAN ===");
    const char *robot_id = "dummyrobot01";
    uint8_t robot_id_len = (uint8_t)strlen(robot_id);

    // Memastikan mutex awal teralokasi
    ml_stream_init();

    // Tunggu status koneksi Wi-Fi terhubung
    xEventGroupWaitBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    
    ESP_LOGI(TAG_WS, "Wi-Fi Terverifikasi Konek, Menginisialisasi Client WebSocket...");
    ml_init_websocket();

    for (;;) {
        bool ws_connected = (s_ws_client != NULL) && esp_websocket_client_is_connected(s_ws_client);

        // BILA WEBSOCKET BELUM CONNECT: KOSONGKAN QUEUE AGAR BUFFER KAMERA TIDAK PENUH
        if (!ws_connected) {
            camera_fb_t *dummy_fb = NULL;
            while (xQueueReceive(frame_queue, &dummy_fb, 0) == pdTRUE) {
                if (dummy_fb != NULL) {
                    esp_camera_fb_return(dummy_fb);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // BILA WEBSOCKET TERHUBUNG: FLUSH FRAME LAMA KECUALI FRAME TERBARU
        while (uxQueueMessagesWaiting(frame_queue) > 1) {
            camera_fb_t *old_fb = NULL;
            if (xQueueReceive(frame_queue, &old_fb, 0) == pdTRUE && old_fb != NULL) {
                esp_camera_fb_return(old_fb);
            }
        }

        // AMBIL FRAME TERBARU DARI QUEUE
        camera_fb_t *fb = NULL;
        if (xQueueReceive(frame_queue, &fb, pdMS_TO_TICKS(50)) == pdTRUE && fb != NULL) {
            
            uint16_t distance_mm = 0;
            bool distance_valid = false;
            ml_get_distance(&distance_mm, &distance_valid);
            uint8_t is_dekat = (distance_valid && distance_mm <= EYE_DISTANCE_THRESHOLD_MM) ? 1 : 0;

            // Efisiensi Alokasi Memori: Hanya realokasi jika ukuran frame bertambah
            size_t required_packet_size = 1 + robot_id_len + 1 + fb->len;
            if (s_tx_packet_buffer == NULL || s_tx_packet_buffer_size < required_packet_size) {
                uint8_t *new_buf = (uint8_t *)realloc(s_tx_packet_buffer, required_packet_size);
                if (new_buf == NULL) {
                    ESP_LOGE(TAG_WS, "Gagal mengalokasikan realloc paket buffer!");
                    esp_camera_fb_return(fb);
                    vTaskDelay(pdMS_TO_TICKS(10));
                    continue;
                }
                s_tx_packet_buffer = new_buf;
                s_tx_packet_buffer_size = required_packet_size;
            }

            // Menyusun Struktur Payload Biner
            s_tx_packet_buffer[0] = robot_id_len;
            memcpy(s_tx_packet_buffer + 1, robot_id, robot_id_len);
            s_tx_packet_buffer[1 + robot_id_len] = is_dekat;
            memcpy(s_tx_packet_buffer + 1 + robot_id_len + 1, fb->buf, fb->len);

            // Transmisi biner ke Server WebSocket
            int sent = esp_websocket_client_send_bin(s_ws_client, (char *)s_tx_packet_buffer, required_packet_size, pdMS_TO_TICKS(100));
            if (sent < 0) {
                ESP_LOGE(TAG_WS, "Gagal mengirim data biner WebSocket (error %d)", sent);
            }

            // Kembalikan Frame Buffer ke Driver Kamera
            esp_camera_fb_return(fb);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================================
// TASK KONEKSI WI-FI
// ============================================================================
void vTaskWifiConnect(void *pvParameter) {
    while (connect_wifi() != ESP_OK) {
        ESP_LOGW("WIFI", "Wi-Fi belum terhubung, mencoba ulang...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("WIFI", "Wi-Fi Berhasil Terhubung!");
    xEventGroupSetBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT);

    esp_wifi_set_ps(WIFI_PS_NONE); // Matikan power saving Wi-Fi untuk latensi minimal
    vTaskDelete(NULL);
}