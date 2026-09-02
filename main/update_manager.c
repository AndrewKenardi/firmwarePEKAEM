#include "update_manager.h"
#include "ota_task.h"
#include "c3_programmer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_config.h"

static const char *TAG = "UPDATE_MGR";
// URL C3 sekarang di-define di dalam c3_programmer.c sendiri (dekat dengan
// kode yang memakainya), tidak perlu diulang di sini.

#define UPDATE_CHECK_INTERVAL_MS (60 * 60 * 1000) // 1 jam kalau normal
#define UPDATE_RETRY_INTERVAL_MS (5 * 60 * 1000)   // 5 menit kalau ada kegagalan

void vTaskUpdateManager(void *pvParameters)
{

    for (;;) {
        bool any_failure = false;

        // 1. Cek update ESP32-S sendiri DULU. Kalau ada update, restart
        //    terjadi di dalam start_ota_update() -- baris berikutnya
        //    tidak akan tercapai.
        ESP_LOGI(TAG, "Cek update firmware ESP32-S...");
        esp_err_t self_ret = start_ota_update(SELF_OTA_URL);
        if (self_ret != ESP_OK) {
            ESP_LOGW(TAG, "Cek update ESP32-S gagal: %s", esp_err_to_name(self_ret));
            any_failure = true;
        }

        uint32_t delay_ms = any_failure ? UPDATE_RETRY_INTERVAL_MS : UPDATE_CHECK_INTERVAL_MS;
        ESP_LOGI(TAG, "Cek berikutnya dalam %lu menit", (unsigned long)(delay_ms / 60000));
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}