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
// ----------------------------------------------------------------------------
// Task khusus untuk membaca sensor VL53L0X
// ----------------------------------------------------------------------------
void vTaskVL53L0X(void *pvParameters) {
    // 1. Reset Hardware VL53L0X via XSHUT
    pinMode(VL53L0X_XSHUT_PIN, OUTPUT);
    digitalWrite(VL53L0X_XSHUT_PIN, LOW);   // Masuk mode Shutdown
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(VL53L0X_XSHUT_PIN, HIGH);  // Aktifkan Sensor
    vTaskDelay(pdMS_TO_TICKS(100));         // Tunggu bootloader sensor siap

    // 2. Configure Internal Pull-Up untuk I2C Sensor
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, INPUT_PULLUP);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Inisialisasi Bus I2C Wire (SDA=15, SCL=13, Freq=100kHz)
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);

    // 4. I2C Scanner Debugging
    ESP_LOGI("I2C_SCAN", "Memulai Pemindaian I2C pada SDA:%d SCL:%d...", I2C_SDA_PIN, I2C_SCL_PIN);
    int nDevices = 0;
    for (byte address = 1; address < 127; address++) {
        Wire.beginTransmission(address);
        byte error = Wire.endTransmission();

        if (error == 0) {
            ESP_LOGI("I2C_SCAN", "Perangkat I2C DITEMUKAN pada alamat 0x%02X !", address);
            nDevices++;
        }
    }

    if (nDevices == 0) {
        ESP_LOGE("I2C_SCAN", "TIDAK ADA Perangkat I2C ditemukan pada SDA:%d SCL:%d!", I2C_SDA_PIN, I2C_SCL_PIN);
        vTaskDelete(NULL);
        return;
    }

    // 5. Inisialisasi Sensor VL53L0X pada Alamat Default (0x29)
    if (!lox.begin(0x29, false, &Wire)) {
        ESP_LOGE(TAG_VL53, "Gagal menginisialisasi VL53L0X! (SDA:%d SCL:%d XSHUT:%d)",
                 I2C_SDA_PIN, I2C_SCL_PIN, VL53L0X_XSHUT_PIN);
        vTaskDelete(NULL);
        return;
    }

    lox.startRangeContinuous();
    ESP_LOGI(TAG_VL53, "Sensor VL53L0X BERHASIL Diaktifkan!");

    // 6. Loop Pembacaan Sensor Continuous
    for (;;) {
        if (lox.isRangeComplete()) {
            uint16_t range = lox.readRange();
            
            // Filter pembacaan valid (< 8000 mm)
            bool is_valid = (range < 8000); 
            if (is_valid) {
                ESP_LOGD(TAG_VL53, "Distance: %d mm", range);
                ml_stream_set_distance(range, true);
            } else {
                ml_stream_set_distance(0, false);
            }
        }

        // Sampling Rate 50ms (~20 FPS)
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
  
  // Set nilai GPIO 23 ke kondisi LOW
    // digitalWrite(23, LOW);

    // =========================================================================
    // 1. ALOKASI OBJECT SYNCHRONIZATION (Harus Pertama)
    // =========================================================================
    wifiEventGroup   = xEventGroupCreateStatic(&wifiEventGroupBuffer);
    cameraEventGroup = xEventGroupCreateStatic(&cameraEventGroupBuffer);

    ml_stream_init(); 

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
    // 2. INIT DRIVER HARDWARE (Retry hingga 5x)
    // =========================================================================
    const int max_retries = 5;
    bool camera_initialized = false;

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        ESP_LOGI(TAG_CAMERA, "Mencoba inisialisasi kamera (Percobaan %d/%d)...", attempt, max_retries);
        
        if (init_camera_driver() == ESP_OK) {
            camera_initialized = true;
            break;
        }

        ESP_LOGW(TAG_CAMERA, "Gagal inisialisasi kamera pada percobaan %d.", attempt);
        
        // Beri jeda 500ms antar percobaan agar hardware/power stabil
        if (attempt < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (camera_initialized) {
        ESP_LOGI(TAG_CAMERA, "Camera Init Berhasil!");
        xEventGroupSetBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    } else {
        ESP_LOGE(TAG_CAMERA, "Camera Init GAGAL setelah %d kali percobaan! Melanjutkan sistem tanpa kamera...", max_retries);
        xEventGroupClearBits(cameraEventGroup, IS_CAMERA_CONNECTED_BIT);
    }

    // =========================================================================
    // 3. WIFICONNECT & CREATION OF CONSUMER TASKS (Tetap berjalan meski cam gagal)
    // =========================================================================
    // Prioritas 20: Jalankan Task Wi-Fi terlebih dahulu
xTaskCreate(vTaskWifiConnect, "taskWifiConnect", 4096, NULL, 20, NULL);

    // Tunggu hingga Wi-Fi benar-benar terhubung dan dapat IP sebelum jalankan UDP Logger
    ESP_LOGI(TAG_MAIN, "Menunggu koneksi Wi-Fi...");
    EventBits_t bits = xEventGroupWaitBits(
        wifiEventGroup,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG_MAIN, "Wi-Fi Terhubung! Menginisialisasi UDP Logger...");
        udp_logger_config_t log_cfg = {
            .server_ip       = "10.38.223.156",
            .server_port     = 5005,
            .queue_len       = 32,
            .sender_priority = 3,
        };
        udp_logger_init(&log_cfg);
    } else {
        ESP_LOGE(TAG_MAIN, "Wi-Fi Gagal Terhubung dalam %d ms! UDP Logger dilewati.", WIFI_CONNECT_TIMEOUT_MS);
    }

    // TASK ML STREAM
    BaseType_t res = xTaskCreatePinnedToCore(vTaskMLStream, "taskMLStream", 8192, NULL, 16, NULL, 0);
    if (res != pdPASS) {
        ESP_LOGE(TAG_MAIN, "GAGAL MEMBUAT taskMLStream! Error code: %d (Kehabisan Heap RAM)", res);
    } else {
        ESP_LOGI(TAG_MAIN, "BERHASIL MEMBUAT taskMLStream!");
    }

    // TASK UPDATE MANAGER (Self OTA)
    BaseType_t res_update = xTaskCreate(vTaskUpdateManager, "taskUpdateManager", 8192, NULL, 20, NULL);
    if (res_update != pdPASS) {
        ESP_LOGE(TAG_MAIN, "GAGAL MEMBUAT taskUpdateManager! Error: %d, Free heap: %lu",
                 res_update, (unsigned long)esp_get_free_heap_size());
    } else {
        ESP_LOGI(TAG_MAIN, "BERHASIL MEMBUAT taskUpdateManager!");
    }

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
    vTaskDelay(pdTICKS_TO_MS(10000));

    xTaskCreate(vTaskVL53L0X, "taskVL53L0X", 3072, NULL, 14, NULL);
    // vTaskDelay(5000);
    xTaskCreate(master_ota_task, "MasterOtaTask", 3072, NULL, 10, NULL);
}