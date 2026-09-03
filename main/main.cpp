// ============================================================================
// C++ / Arduino Libraries
// ============================================================================
#include "Arduino.h"
#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// ============================================================================
// C Libraries & ESP-IDF Components
// ============================================================================
extern "C" {
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include "esp_heap_caps.h"


// FreeRTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

// IO & Drivers
#include "driver/gpio.h"

// Camera
#include "esp_camera.h"
#include "pins.h"

// Logging
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"

// WiFi & Networking
#include "connect_wifi.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "udpLogger.h"
#include "ota_task.h"

// Project tasks/handlers
#include "wifiStreamTask.h"
#include "cameraTask.h"
#include "taskHandlers.h"
#include "c3_programmer.h"
}

#ifndef WIFI_CONNECTED_BIT
#define WIFI_CONNECTED_BIT BIT0
#endif

#ifndef IS_CAMERA_CONNECTED_BIT
#define IS_CAMERA_CONNECTED_BIT BIT0
#endif

#define WIFI_CONNECT_TIMEOUT_MS 15000

// ----------------------------------------------------------------------------
// Global state & Objects
// ----------------------------------------------------------------------------
QueueHandle_t frame_queue = NULL;

StaticEventGroup_t wifiEventGroupBuffer;
EventGroupHandle_t wifiEventGroup;

StaticEventGroup_t cameraEventGroupBuffer;
EventGroupHandle_t cameraEventGroup;

static const char *TAG_CAMERA = "CAMERA";
static const char *TAG_MAIN   = "MAIN";
static const char *TAG_VL53   = "JARAK";

Adafruit_VL53L0X lox = Adafruit_VL53L0X();

// ----------------------------------------------------------------------------
// Task khusus untuk membaca sensor VL53L0X
// ----------------------------------------------------------------------------
void vTaskVL53L0X(void *pvParameters) {
    // Reset Hardware Sensor via XSHUT (Non-blocking menggunakan FreeRTOS delay)
    pinMode(VL53L0X_XSHUT_PIN, OUTPUT);
    digitalWrite(VL53L0X_XSHUT_PIN, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(VL53L0X_XSHUT_PIN, HIGH);
    vTaskDelay(pdMS_TO_TICKS(10));

    // Inisialisasi I2C Wire untuk Arduino Library
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    if (!lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
        ESP_LOGE(TAG_VL53, "Gagal menginisialisasi VL53L0X! Cek wiring SDA=%d SCL=%d XSHUT=%d",
                 I2C_SDA_PIN, I2C_SCL_PIN, VL53L0X_XSHUT_PIN);
        vTaskDelete(NULL); // Hapus task jika hardware tidak terdeteksi
        return;
    }

    lox.startRangeContinuous();
    ESP_LOGI(TAG_VL53, "Sensor VL53L0X berhasil dimulai!");

    for (;;) {
        if (lox.isRangeComplete()) {
            uint16_t range = lox.readRange();
            
            // Filter nilai pembacaan valid (VL53L0X return 8190/8191 jika out of range)
            bool is_valid = (range < 8000); 
            if (is_valid) {
                ESP_LOGD(TAG_VL53, "Distance: %d mm", range);
                ml_stream_set_distance(range, true);
            } else {
                ml_stream_set_distance(0, false);
            }
        }

        // Sampling rate 50ms (~20 FPS) agar realtime sinkron dengan Frame Kamera
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

extern "C" void app_main()
{
    ota_confirm_running_app();
    initArduino();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // =========================================================================
    // 1. ALOKASI OBJECT SYNCHRONIZATION (Harus Pertama)
    // =========================================================================
    wifiEventGroup   = xEventGroupCreateStatic(&wifiEventGroupBuffer);
    cameraEventGroup = xEventGroupCreateStatic(&cameraEventGroupBuffer);

    ml_stream_init(); // Inisialisasi s_distance_mutex

    if (camera_capture_mutex == NULL) {
        camera_capture_mutex = xSemaphoreCreateMutex();
        assert(camera_capture_mutex != NULL);
    }

    frame_queue = xQueueCreate(1, sizeof(camera_fb_t *));
    if (frame_queue == NULL) {
        ESP_LOGE(TAG_MAIN, "Gagal membuat frame_queue!");
        return;
    }

    // =========================================================================
    // 2. INIT DRIVER HARDWARE
    // =========================================================================
    if (init_camera_driver() != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "Camera Init Failed!");
        xEventGroupClearBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    } else {
        ESP_LOGI(TAG_CAMERA, "Camera Init Berhasil!");
        xEventGroupSetBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    }

    // =========================================================================
    // 3. WIFICONNECT & CREATION OF CONSUMER TASKS
    // =========================================================================
    // Prioritas 20: Jalankan Task Wi-Fi terlebih dahulu
    xTaskCreate(vTaskWifiConnect, "taskWifiConnect", 3072, NULL, 20, NULL);

    // Buka UDP Logger
    // udp_logger_config_t log_cfg = {
    //     .server_ip       = "10.45.173.156",
    //     .server_port     = 5005,
    //     .queue_len       = 32,
    //     .sender_priority = 3,
    // };
    // udp_logger_init(&log_cfg);

    // TASK ML STREAM: Menggunakan Stack Size 8192 (Aman untuk TLS/SSL Handshake WSS)
    BaseType_t res = xTaskCreatePinnedToCore(vTaskMLStream, "taskMLStream", 8192, NULL, 5, NULL, 1);
    if (res != pdPASS) {
        ESP_LOGE(TAG_MAIN, "GAGAL MEMBUAT taskMLStream! Error code: %d (Kehabisan Heap RAM)", res);
    } else {
        ESP_LOGI(TAG_MAIN, "BERHASIL MEMBUAT taskMLStream!");
    }

    // xTaskCreate(vTaskUpdateManager, "taskUpdateManager", 4096, NULL, 5, NULL);

    // =========================================================================
    // 4. TASK PERIPHERAL LAIN
    // =========================================================================
    EventBits_t cam_bits = xEventGroupGetBits(cameraEventGroup);
    if (cam_bits & IS_CAMERA_CONNECTED_BIT) {
        xTaskCreateStaticPinnedToCore(
            vTaskCameraRead, "taskCameraRead", CAMERA_STACK_SIZE, NULL, 15,
            xCameraReadStack, &xCameraReadTaskBuffer, 1);
    } else {
        ESP_LOGW(TAG_MAIN, "Melewati pembuatan taskCameraRead karena Kamera tidak terdeteksi.");
    }

ESP_LOGI("HEAPP", "PSRAM total: %lu, PSRAM free: %lu",
         (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
         (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    xTaskCreate(vTaskVL53L0X, "taskVL53L0X", 3072, NULL, 5, NULL);
}