#include "otaTask.h"
#include <string.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG_OTA = "OTA";

// TODO: ganti dengan URL server firmware kamu (https disarankan)
#define OTA_URL "http://192.168.1.17:5000/firmwarePkm.bin"

// Interval pengecekan OTA otomatis (ms)
#define OTA_CHECK_INTERVAL_MS (60 * 60 * 1000) // 1 jam

esp_err_t start_ota_update(const char *url)
{
    ESP_LOGI(TAG_OTA, "Mulai OTA dari %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL, // ganti dgn esp_crt_bundle_attach jika pakai bundle CA
        .skip_cert_common_name_check = false,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_config, &ota_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_OTA, "esp_https_ota_begin gagal: %s", esp_err_to_name(ret));
        return ret;
    }

    // Validasi versi firmware baru vs yang sedang berjalan (opsional tapi disarankan)
    esp_app_desc_t new_app_info;
    ret = esp_https_ota_get_img_desc(ota_handle, &new_app_info);
    if (ret == ESP_OK) {
        const esp_app_desc_t *running_app_info = esp_app_get_description();
        ESP_LOGI(TAG_OTA, "Versi berjalan : %s", running_app_info->version);
        ESP_LOGI(TAG_OTA, "Versi baru     : %s", new_app_info.version);

        if (strcmp(new_app_info.version, running_app_info->version) == 0) {
            ESP_LOGW(TAG_OTA, "Versi sama, OTA dibatalkan");
            esp_https_ota_abort(ota_handle);
            return ESP_FAIL;
        }
    }

    // Loop download + tulis ke partisi OTA
    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        // bisa log progres di sini jika perlu:
        // ESP_LOGD(TAG_OTA, "Image bytes read: %d", esp_https_ota_get_image_len_read(ota_handle));
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG_OTA, "Data firmware belum lengkap diterima");
        esp_https_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    esp_err_t finish_ret = esp_https_ota_finish(ota_handle);
    if (finish_ret != ESP_OK) {
        if (finish_ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG_OTA, "Image OTA gagal validasi");
        }
        ESP_LOGE(TAG_OTA, "OTA gagal: %s", esp_err_to_name(finish_ret));
        return finish_ret;
    }

    ESP_LOGI(TAG_OTA, "OTA berhasil, restart perangkat...");
    esp_restart();

    return ESP_OK; // tidak akan pernah sampai sini
}

void vTaskOtaCheck(void *pvParameters)
{
    for (;;) {
        esp_err_t err = start_ota_update(OTA_URL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG_OTA, "OTA check gagal (%s), coba lagi nanti", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}