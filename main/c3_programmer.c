#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_crc.h"
#include "esp_http_client.h"
#include "fw_version.h"
#include "c3_programmer.h"

static const char *TAG = "MASTER_OTA";

// Konfigurasi URL HTTP Server Lokal
#define FIRMWARE_HTTP_URL     "http://10.38.223.156:8000/ESP-C3_Firmware.bin"

static const uint8_t SYNC_BYTES[6] = {0xC0, 0xFF, 0xFE, 0xAA, 0x55, 0x90};
#define ACK_BYTE              0x06
#define SKIP_BYTE             0x15   
#define OTA_DONE_BYTE         0x99   
#define FINAL_ACK_BYTE        0xA5   
#define OTA_ERROR_BYTE        0xE7   

#define LAST_CHUNK_TIMEOUT_MS (10000) 
#define LAST_CHUNK_RETRIES    (5)    

#ifndef CHUNK_SIZE
#define CHUNK_SIZE            (1024)
#endif

#ifndef MAX_RETRIES
#define MAX_RETRIES           (5)
#endif

#pragma pack(push, 1)
typedef struct {
    uint8_t  sync[6];     
    uint32_t fw_size;     
    uint32_t fw_crc32;    
    uint32_t fw_version;  
} ota_header_t;
#pragma pack(pop)

static void master_uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(MASTER_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MASTER_UART_NUM, MASTER_TX_PIN, MASTER_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(MASTER_UART_NUM, 4096, 4096, 0, NULL, 0));
}

static void master_send_byte_blocking(uint8_t byte)
{
    uart_write_bytes(MASTER_UART_NUM, (const char *)&byte, 1);
    uart_wait_tx_done(MASTER_UART_NUM, pdMS_TO_TICKS(200));
}

static const char *ota_error_to_str(uint8_t code)
{
    switch (code) {
        case 0x01: return "Partisi OTA target tidak ditemukan di slave";
        case 0x02: return "Partisi target sama dengan partisi yang sedang berjalan di slave";
        case 0x03: return "esp_ota_begin() gagal di slave";
        case 0x04: return "Slave gagal menulis flash (esp_ota_write gagal)";
        case 0x05: return "Slave timeout menunggu chunk data";
        case 0x06: return "CRC32 tidak cocok di slave";
        case 0x07: return "Slave gagal finalisasi OTA";
        default:   return "Kode error tidak dikenal";
    }
}

static bool master_check_error(uint8_t first_byte)
{
    if (first_byte != OTA_ERROR_BYTE) {
        return false;
    }

    uint8_t code = 0;
    int len = uart_read_bytes(MASTER_UART_NUM, &code, 1, pdMS_TO_TICKS(1000));

    ESP_LOGE(TAG, "==========================================");
    if (len > 0) {
        ESP_LOGE(TAG, " SLAVE MELAPORKAN ERROR: %s (0x%02X)", ota_error_to_str(code), code);
    } else {
        ESP_LOGE(TAG, " Sinyal error diterima, tapi kode error timeout.");
    }
    ESP_LOGE(TAG, "==========================================");
    return true;
}

static bool wait_for_ota_done_signal(void)
{
    uint8_t rx_buf[1];

    for (int attempt = 1; attempt <= LAST_CHUNK_RETRIES; attempt++) {
        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(LAST_CHUNK_TIMEOUT_MS));

        if (len > 0 && rx_buf[0] == OTA_DONE_BYTE) {
            master_send_byte_blocking(FINAL_ACK_BYTE);
            ESP_LOGI(TAG, "OTA_DONE diterima, FINAL_ACK dibalas ke slave.");
            return true;
        }

        if (len > 0 && master_check_error(rx_buf[0])) {
            return false;
        }

        ESP_LOGW(TAG, "Menunggu konfirmasi OTA_DONE dari Slave... (%d/%d)", attempt, LAST_CHUNK_RETRIES);
    }

    return false;
}

void master_ota_task(void *pvParameters)
{
    master_uart_init();

    // 1. Inisialisasi HTTP Client untuk membuka koneksi ke Server Lokal
    esp_http_client_config_t http_cfg = {
        .url = FIRMWARE_HTTP_URL,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Gagal membuat HTTP client handle!");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Gagal terhubung ke HTTP Server: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    // Mengambil Content-Length dari HTTP Header sebagai ukuran file
    int fw_size = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    if (status_code != 200 || fw_size <= 0) {
        ESP_LOGE(TAG, "HTTP Request Gagal, Status Code: %d, Ukuran File: %d", status_code, fw_size);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    // Membaca Custom Header dari Server (jika ada, e.g. X-Firmware-CRC32 & X-Firmware-Version)
    uint32_t calculated_crc = 0;
    uint32_t target_fw_ver = FIRMWARE_VERSION;

    char header_val[32] = {0};
    if (esp_http_client_get_header(client, "X-Firmware-CRC32", (char**)&header_val) == ESP_OK && header_val[0] != 0) {
        calculated_crc = (uint32_t)strtoul(header_val, NULL, 16);
    }
    memset(header_val, 0, sizeof(header_val));
    if (esp_http_client_get_header(client, "X-Firmware-Version", (char**)&header_val) == ESP_OK && header_val[0] != 0) {
        target_fw_ver = (uint32_t)strtoul(header_val, NULL, 16);
    }

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " Memulai Stream OTA dari Server Lokal");
    ESP_LOGI(TAG, " Ukuran Firmware : %d byte", fw_size);
    ESP_LOGI(TAG, " Target CRC32    : 0x%08X", (unsigned int)calculated_crc);
    ESP_LOGI(TAG, " Versi Firmware  : 0x%08X", (unsigned int)target_fw_ver);
    ESP_LOGI(TAG, "==========================================");

    // 2. Kirim Header Handshake ke Slave via UART
    ota_header_t header;
    memcpy(header.sync, SYNC_BYTES, sizeof(SYNC_BYTES));
    header.fw_size    = (uint32_t)fw_size;
    header.fw_crc32   = calculated_crc;
    header.fw_version = target_fw_ver;

    uint8_t rx_buf[1];
    bool synced = false;
    bool skip_update = false;

    uart_flush_input(MASTER_UART_NUM);

    while (!synced) {
        ESP_LOGI(TAG, "Mengirim Sync Header ke Slave...");
        uart_write_bytes(MASTER_UART_NUM, (const char *)&header, sizeof(ota_header_t));
        uart_wait_tx_done(MASTER_UART_NUM, pdMS_TO_TICKS(200));

        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(5000));

        if (len > 0 && rx_buf[0] == ACK_BYTE) {
            synced = true;
            ESP_LOGI(TAG, "Handshake Sukses! Slave siap menerima chunk.");
            vTaskDelay(pdMS_TO_TICKS(200)); 
        } else if (len > 0 && rx_buf[0] == SKIP_BYTE) {
            ESP_LOGI(TAG, "Versi Slave sudah sama. OTA dilewati.");
            skip_update = true;
            synced = true; 
        } else if (len > 0 && master_check_error(rx_buf[0])) {
            ESP_LOGE(TAG, "OTA dibatalkan karena error di slave saat handshake.");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            vTaskDelete(NULL);
            return;
        } else {
            ESP_LOGW(TAG, "Timeout ACK dari Slave, mencoba ulang...");
            uart_flush_input(MASTER_UART_NUM);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (skip_update) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    // 3. Streaming Loop: Baca dari HTTP -> Kirim ke UART
    uint8_t chunk_buf[CHUNK_SIZE];
    size_t bytes_sent = 0;
    int retry_count = 0;

    while (bytes_sent < (size_t)fw_size) {
        size_t send_len = (size_t)fw_size - bytes_sent;
        if (send_len > CHUNK_SIZE) {
            send_len = CHUNK_SIZE;
        }

        // Baca 1 Chunk langsung dari Server HTTP
        int bytes_read = esp_http_client_read(client, (char *)chunk_buf, send_len);
        if (bytes_read <= 0) {
            ESP_LOGE(TAG, "Gagal/Timeout saat membaca data dari HTTP Server!");
            break;
        }

        bool is_last_chunk = (bytes_sent + bytes_read >= (size_t)fw_size);

        // Meneruskan chunk data ke UART Slave
        uart_write_bytes(MASTER_UART_NUM, (const char *)chunk_buf, bytes_read);
        uart_wait_tx_done(MASTER_UART_NUM, pdMS_TO_TICKS(500));

        // Menunggu ACK dari Slave
        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(5000));
        if (len > 0 && rx_buf[0] == ACK_BYTE) {
            bytes_sent += bytes_read;
            retry_count = 0;

            int progress = (bytes_sent * 100) / fw_size;
            ESP_LOGI(TAG, "Progres Streaming: %d%% (%u/%d byte)", progress, (unsigned int)bytes_sent, fw_size);

            if (is_last_chunk) {
                ESP_LOGI(TAG, "Chunk terakhir dikirim. Menunggu verifikasi & finalisasi OTA...");

                if (wait_for_ota_done_signal()) {
                    ESP_LOGI(TAG, "OTA SUKSES - Slave terkonfirmasi valid & akan reboot.");
                } else {
                    ESP_LOGE(TAG, "OTA TIDAK SELESAI: gagal mendapat konfirmasi dari slave.");
                }
            }
        } else if (len > 0 && master_check_error(rx_buf[0])) {
            ESP_LOGE(TAG, "OTA dibatalkan karena error di slave pada offset %u.", (unsigned int)bytes_sent);
            break;
        } else {
            retry_count++;
            ESP_LOGW(TAG, "Timeout ACK pada offset %u (Percobaan %d/%d)", (unsigned int)bytes_sent, retry_count, MAX_RETRIES);

            if (retry_count >= MAX_RETRIES) {
                ESP_LOGE(TAG, "Gagal Mengirim OTA: Slave tidak merespon!");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Tutup & bersihkan koneksi HTTP Client
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Task OTA Master selesai.");
    vTaskDelete(NULL);
}