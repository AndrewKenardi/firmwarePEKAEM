// ============================================================================
// CLib
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include "esp_heap_caps.h"

// ============================================================================
// FreeRTOS
// ============================================================================
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// ============================================================================
// IO
// ============================================================================
#include "driver/gpio.h"

// ============================================================================
// Camera
// ============================================================================
#include "esp_camera.h"
#include "pins.h"

// ============================================================================
// Logging
// ============================================================================
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"

// ============================================================================
// WiFi
// ============================================================================
#include "connect_wifi.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "udpLogger.h"
#include "ota_task.h"

// ============================================================================
// Project tasks/handlers
// ============================================================================
#include "wifiStreamTask.h"
#include "cameraTask.h"
#include "taskHandlers.h"
#include "c3_programmer.h"
#include "update_manager.h"

#define KB(x) ((x) * 1024)

#ifndef WIFI_CONNECTED_BIT
#define WIFI_CONNECTED_BIT BIT0
#endif

#ifndef IS_CAMERA_CONNECTED_BIT
#define IS_CAMERA_CONNECTED_BIT BIT0
#endif

#define WIFI_CONNECT_TIMEOUT_MS 15000

// ----------------------------------------------------------------------------
// Global state
// ----------------------------------------------------------------------------
QueueHandle_t frame_queue = NULL;

StaticEventGroup_t wifiEventGroupBuffer;
EventGroupHandle_t wifiEventGroup;

StaticEventGroup_t cameraEventGroupBuffer;
EventGroupHandle_t cameraEventGroup;

static const char *TAG_CAMERA = "CAMERA";
static const char *TAG_MAIN   = "MAIN";

void app_main(void)
{
    ota_confirm_running_app();
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifiEventGroup   = xEventGroupCreateStatic(&wifiEventGroupBuffer);
    cameraEventGroup = xEventGroupCreateStatic(&cameraEventGroupBuffer);

    // Alokasi memori untuk frame_queue sebelum task kamera dijalankan
    frame_queue = xQueueCreate(1, sizeof(camera_fb_t *));
    if (frame_queue == NULL) {
        ESP_LOGE(TAG_MAIN, "Gagal membuat frame_queue! Kehabisan memori.");
        return;
    }
    ESP_LOGI(TAG_MAIN, "frame_queue berhasil dibuat.");

    xTaskCreate(vTaskWifiConnect, "taskWifiConnect", 3072, NULL, 20, NULL);

    ESP_LOGI(TAG_MAIN, "Menunggu koneksi WiFi (timeout %d ms)...", WIFI_CONNECT_TIMEOUT_MS);
    EventBits_t wifi_bits = xEventGroupWaitBits(
        wifiEventGroup, WIFI_CONNECTED_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    bool wifi_connected = (wifi_bits & WIFI_CONNECTED_BIT) != 0;

    if (wifi_connected) {
        ESP_LOGI(TAG_MAIN, "WiFi terhubung, lanjut init UDP logger & OTA");

        udp_logger_config_t log_cfg = {
            .server_ip       = "10.45.173.156",
            .server_port     = 5005,
            .queue_len       = 32,
            .sender_priority = 3,
        };
        esp_err_t log_ret = udp_logger_init(&log_cfg);
        if (log_ret == ESP_OK) {
            ESP_LOGI(TAG_MAIN, "UDP logger aktif, ini pesan test dari ESP32!");
        } else {
            ESP_LOGE(TAG_MAIN, "UDP logger gagal init: %s", esp_err_to_name(log_ret));
        }
        
        xTaskCreate(vTaskUpdateManager, "taskUpdateManager", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGE(TAG_MAIN, "WiFi TIDAK terhubung dalam %d ms -- UDP logger & OTA dilewati",
                 WIFI_CONNECT_TIMEOUT_MS);
    }    

    // Inisialisasi driver kamera
    if (init_camera_driver() != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "Camera Init Failed!");
        xEventGroupClearBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG_CAMERA, "Camera Init Berhasil!");
        xEventGroupSetBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    }

    xTaskCreate(master_ota_task, "masterOtaTask", 4096, NULL, 5, NULL);

    // Jalankan Task Pembaca Kamera HANYA jika inisialisasi driver kamera sukses
    EventBits_t cam_bits = xEventGroupGetBits(cameraEventGroup);
    if (cam_bits & IS_CAMERA_CONNECTED_BIT) {
        xTaskCreateStaticPinnedToCore(
            vTaskCameraRead, "taskCameraRead", CAMERA_STACK_SIZE, NULL, 15,
            xCameraReadStack, &xCameraReadTaskBuffer, 1);
    } else {
        ESP_LOGW(TAG_MAIN, "Kamera gagal init, vTaskCameraRead TIDAK dijalankan.");
    }
}