#include <assert.h>
#include "vl53l0x_task.h"

extern "C" {
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_system.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_camera.h"
#include "pins.h"

#include "connect_wifi.h"
#include "udpLogger.h"
#include "ota_task.h"

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

// ============================================================================
// GLOBAL STATE
// ============================================================================

QueueHandle_t frame_queue = NULL;

StaticEventGroup_t wifiEventGroupBuffer;
EventGroupHandle_t wifiEventGroup;

StaticEventGroup_t cameraEventGroupBuffer;
EventGroupHandle_t cameraEventGroup;

static const char *TAG_CAMERA = "CAMERA";
static const char *TAG_MAIN   = "MAIN";

// void vTaskVL53L0X(void *pvParameters) {
//     // Reset Hardware Sensor via XSHUT (Non-blocking menggunakan FreeRTOS delay)
//     pinMode(VL53L0X_XSHUT_PIN, OUTPUT);
//     digitalWrite(VL53L0X_XSHUT_PIN, LOW);
//     vTaskDelay(pdMS_TO_TICKS(10));
//     digitalWrite(VL53L0X_XSHUT_PIN, HIGH);
//     vTaskDelay(pdMS_TO_TICKS(10));

//     // Inisialisasi I2C Wire untuk Arduino Library
//     Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

//     if (!lox.begin(VL53L0X_I2C_ADDR, false, &Wire)) {
//         ESP_LOGE(TAG_VL53, "Gagal menginisialisasi VL53L0X! Cek wiring SDA=%d SCL=%d XSHUT=%d",
//                  I2C_SDA_PIN, I2C_SCL_PIN, VL53L0X_XSHUT_PIN);
//         vTaskDelete(NULL); // Hapus task jika hardware tidak terdeteksi
//         return;
//     }

//     lox.startRangeContinuous();
//     ESP_LOGI(TAG_VL53, "Sensor VL53L0X berhasil dimulai!");

//     for (;;) {
//         if (lox.isRangeComplete()) {
//             uint16_t range = lox.readRange();
            
//             // Filter nilai pembacaan valid (VL53L0X return 8190/8191 jika out of range)
//             bool is_valid = (range < 8000); 
//             if (is_valid) {
//                 ESP_LOGD(TAG_VL53, "Distance: %d mm", range);
//                 ml_stream_set_distance(range, true);
//             } else {
//                 ml_stream_set_distance(0, false);
//             }
//         }

//         // Sampling rate 50ms (~20 FPS) agar realtime sinkron dengan Frame Kamera
//         vTaskDelay(pdMS_TO_TICKS(50));
//     }
// }

// ============================================================================
// MAIN PROGRAM
// ============================================================================

extern "C" void app_main()
{
    // Jangan panggil initArduino() di sini.
    // Arduino hanya dipakai oleh WebSocket di wifiStreamTask.cpp.
    // Sensor VL53L0X memakai ESP-IDF I2C native di vl53l0x_task.c.

    ota_confirm_running_app();

    // ========================================================================
    // 1. INISIALISASI NVS
    // ========================================================================

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // ========================================================================
    // 2. INISIALISASI NETWORK
    // ========================================================================

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ========================================================================
    // 3. EVENT GROUP WI-FI DAN KAMERA
    // ========================================================================

    wifiEventGroup = xEventGroupCreateStatic(&wifiEventGroupBuffer);

    cameraEventGroup = xEventGroupCreateStatic(
        &cameraEventGroupBuffer
    );

    // Menyediakan mutex dan penyimpanan nilai jarak untuk ML stream
    ml_stream_init();

    // ========================================================================
    // 4. MUTEX KAMERA
    // ========================================================================

    if (camera_capture_mutex == NULL) {
        camera_capture_mutex = xSemaphoreCreateMutex();
        assert(camera_capture_mutex != NULL);
    }

    // ========================================================================
    // 5. QUEUE FRAME KAMERA
    // ========================================================================

    frame_queue = xQueueCreate(1, sizeof(camera_fb_t *));

    if (frame_queue == NULL) {
        ESP_LOGE(TAG_MAIN, "Gagal membuat frame_queue.");
        return;
    }

    // ========================================================================
    // 6. INISIALISASI KAMERA DENGAN RETRY
    // ========================================================================

    const int max_retries = 5;
    bool camera_initialized = false;

    for (int attempt = 1; attempt <= max_retries; attempt++) {
        ESP_LOGI(
            TAG_CAMERA,
            "Inisialisasi kamera (%d/%d)",
            attempt,
            max_retries
        );

        if (init_camera_driver() == ESP_OK) {
            camera_initialized = true;
            break;
        }

        ESP_LOGW(
            TAG_CAMERA,
            "Kamera gagal pada percobaan %d.",
            attempt
        );

        if (attempt < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    if (camera_initialized) {
        ESP_LOGI(TAG_CAMERA, "Kamera berhasil diinisialisasi.");

        xEventGroupSetBits(
            cameraEventGroup,
            IS_CAMERA_CONNECTED_BIT
        );
    } else {
        ESP_LOGE(
            TAG_CAMERA,
            "Kamera gagal setelah %d percobaan.",
            max_retries
        );

        xEventGroupClearBits(
            cameraEventGroup,
            IS_CAMERA_CONNECTED_BIT
        );
    }

    // ========================================================================
    // 7. TASK WI-FI
    // ========================================================================

    xTaskCreate(
        vTaskWifiConnect,
        "taskWifiConnect",
        4096,
        NULL,
        20,
        NULL
    );

    ESP_LOGI(TAG_MAIN, "Menunggu koneksi Wi-Fi...");

    EventBits_t bits = xEventGroupWaitBits(
        wifiEventGroup,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    // ========================================================================
    // 8. UDP LOGGER
    // ========================================================================

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(
            TAG_MAIN,
            "Wi-Fi terhubung. Menginisialisasi UDP Logger."
        );

        udp_logger_config_t log_cfg = {
            .server_ip = "10.38.223.156",
            .server_port = 5005,
            .queue_len = 32,
            .sender_priority = 3,
        };

        udp_logger_init(&log_cfg);
    } else {
        ESP_LOGE(
            TAG_MAIN,
            "Wi-Fi gagal terhubung dalam %d ms. UDP Logger dilewati.",
            WIFI_CONNECT_TIMEOUT_MS
        );
    }

    // ========================================================================
    // 9. TASK WEBSOCKET DAN ML STREAM
    // ========================================================================

    BaseType_t res_ml = xTaskCreatePinnedToCore(
        vTaskMLStream,
        "taskMLStream",
        8192,
        NULL,
        16,
        NULL,
        0
    );

    if (res_ml != pdPASS) {
        ESP_LOGE(
            TAG_MAIN,
            "Gagal membuat taskMLStream. Free heap: %lu",
            (unsigned long)esp_get_free_heap_size()
        );
    } else {
        ESP_LOGI(TAG_MAIN, "taskMLStream berhasil dibuat.");
    }

    // ========================================================================
    // 10. TASK OTA UPDATE
    // ========================================================================

    BaseType_t res_update = xTaskCreate(
        vTaskUpdateManager,
        "taskUpdateManager",
        8192,
        NULL,
        20,
        NULL
    );

    if (res_update != pdPASS) {
        ESP_LOGE(
            TAG_MAIN,
            "Gagal membuat taskUpdateManager. Free heap: %lu",
            (unsigned long)esp_get_free_heap_size()
        );
    } else {
        ESP_LOGI(TAG_MAIN, "taskUpdateManager berhasil dibuat.");
    }

    // ========================================================================
    // 11. TASK PEMBACAAN KAMERA
    // ========================================================================

    EventBits_t cam_bits = xEventGroupGetBits(cameraEventGroup);

    if (cam_bits & IS_CAMERA_CONNECTED_BIT) {
        xTaskCreateStaticPinnedToCore(
            vTaskCameraRead,
            "taskCameraRead",
            CAMERA_STACK_SIZE,
            NULL,
            15,
            xCameraReadStack,
            &xCameraReadTaskBuffer,
            1
        );
    } else {
        ESP_LOGW(
            TAG_MAIN,
            "Task kamera dilewati karena kamera tidak terdeteksi."
        );
    }

    ESP_LOGI(
        "HEAPP",
        "PSRAM total: %lu, PSRAM free: %lu",
        (unsigned long)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    // Beri waktu sistem kamera, Wi-Fi, dan WebSocket untuk stabil.
    // vTaskDelay(pdMS_TO_TICKS(10000));

    // // ========================================================================
    // // 12. TASK SENSOR JARAK VL53L0X
    // //
    // // Kode I2C native ESP-IDF v6 berada di vl53l0x_task.c.
    // // Tidak memakai Wire, Adafruit, atau driver/i2c.h.
    // // ========================================================================

    // BaseType_t res_vl53 = xTaskCreate(
    //     vTaskVL53L0X,
    //     "taskVL53L0X",
    //     4096,
    //     NULL,
    //     14,
    //     NULL
    // );

    // if (res_vl53 != pdPASS) {
    //     ESP_LOGE(TAG_MAIN, "Gagal membuat taskVL53L0X.");
    // } else {
    //     ESP_LOGI(TAG_MAIN, "taskVL53L0X berhasil dibuat.");
    // }

    // // ========================================================================
    // // 13. TASK OTA MASTER
    // // ========================================================================

    // xTaskCreate(
    //     master_ota_task,
    //     "MasterOtaTask",
    //     3072,
    //     NULL,
    //     10,
    //     NULL
    // );
}