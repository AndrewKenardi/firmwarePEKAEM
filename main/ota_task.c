#include "ota_task.h"
#include <string.h>
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include "ota_config.h"


static const char *TAG_OTA = "OTA";
static const char *TAG = "VERSION_CHECK";

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

esp_err_t version_check_fetch(const char *url, char *out_buf, size_t out_buf_size)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return ESP_FAIL;

    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Gagal buka koneksi ke %s: %s", url, esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len <= 0 || (size_t)content_len >= out_buf_size) {
        ESP_LOGW(TAG, "Content-Length tidak valid/terlalu besar: %d", content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int read_len = esp_http_client_read(client, out_buf, content_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        return ESP_FAIL;
    }

    out_buf[read_len] = '\0';

    // Buang whitespace/newline di ujung (umum kalau file dibuat manual echo/editor)
    while (read_len > 0 && (out_buf[read_len - 1] == '\n' || out_buf[read_len - 1] == '\r' || out_buf[read_len - 1] == ' ')) {
        out_buf[--read_len] = '\0';
    }

    return ESP_OK;
}

void ota_confirm_running_app(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG_OTA, "Firmware pending verify -> konfirmasi valid, cancel rollback");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_LOGI(TAG_OTA, "Boot dari partisi: %s @ 0x%x", running->label, (unsigned int)running->address);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG_OTA, "Firmware version: %s | build: %s %s",
             app_desc->version, app_desc->date, app_desc->time);
}

esp_err_t start_ota_update(const char *url)
{
    ESP_LOGI(TAG_OTA, "Mulai OTA dari %s", url);

    esp_http_client_config_t http_config = {
        .url = url,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL,
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

    esp_app_desc_t new_app_info;
    if (esp_https_ota_get_img_desc(ota_handle, &new_app_info) == ESP_OK) {
        const esp_app_desc_t *running_app_info = esp_app_get_description();
        ESP_LOGI(TAG_OTA, "Versi berjalan : %s", running_app_info->version);
        ESP_LOGI(TAG_OTA, "Versi baru     : %s", new_app_info.version);

        if (strcmp(new_app_info.version, running_app_info->version) == 0) {
            ESP_LOGI(TAG_OTA, "Versi sama, tidak ada update (bukan error)");
            esp_https_ota_abort(ota_handle);
            return ESP_OK;
        }
    }

    while (1) {
        ret = esp_https_ota_perform(ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG_OTA, "esp_https_ota_perform gagal: 0x%x (%s)", ret, esp_err_to_name(ret));
        esp_https_ota_abort(ota_handle);
        return ret;
    }

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG_OTA, "Data firmware belum lengkap diterima");
        esp_https_ota_abort(ota_handle);
        return ESP_FAIL;
    }

    esp_err_t finish_ret = esp_https_ota_finish(ota_handle);
    if (finish_ret != ESP_OK) {
        if (finish_ret == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG_OTA, "Image OTA gagal validasi (checksum/signature)");
        }
        ESP_LOGE(TAG_OTA, "OTA gagal: %s", esp_err_to_name(finish_ret));
        return finish_ret;
    }

    ESP_LOGI(TAG_OTA, "OTA berhasil, restart perangkat dalam 1 detik...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}