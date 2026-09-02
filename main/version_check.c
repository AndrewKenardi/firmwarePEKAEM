#include "version_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "VERSION_CHECK";

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