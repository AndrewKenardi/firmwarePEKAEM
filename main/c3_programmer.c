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
#include "net_discovery.h"

static const char *TAG = "MASTER_OTA";

// URL firmware slave (C3) TIDAK di-hardcode lagi -- host-nya didapat
// runtime dari net_discovery_find_server() (lihat net_discovery.c +
// discovery_responder.py di server), supaya tetap jalan walau IP hotspot
// berubah, selama ESP32 & server 1 jaringan.
#define FIRMWARE_HTTP_PORT    8000
#define FIRMWARE_HTTP_PATH    "/ESP-C3_Firmware.bin"

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

// Variabel global untuk menampung kustom header dari HTTP Event Handler
static uint32_t server_calculated_crc = 0;
static uint32_t server_target_fw_ver = 0;

#pragma pack(push, 1)
typedef struct {
    uint8_t  sync[6];     
    uint32_t fw_size;     
    uint32_t fw_crc32;    
    uint32_t fw_version;  
} ota_header_t;
#pragma pack(pop)

// HTTP Event Handler untuk menangkap custom header secara real-time
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcasecmp(evt->header_key, "X-Firmware-CRC32") == 0) {
                // Paksa basis 16 karena string format CRC32 berupa hex murni (contoh: 0BD6814D)
                server_calculated_crc = (uint32_t)strtoul(evt->header_value, NULL, 16);
                ESP_LOGI(TAG, "Header Tertangkap -> CRC32: %s (0x%08X)", evt->header_value, (unsigned int)server_calculated_crc);
            }
            else if (strcasecmp(evt->header_key, "X-Firmware-Version") == 0) {
                // Versi bisa basis 10 atau 16 tergantung format server Anda
                server_target_fw_ver = (uint32_t)strtoul(evt->header_value, NULL, 0);
                ESP_LOGI(TAG, "Header Tertangkap -> Version: %s (0x%08X)", evt->header_value, (unsigned int)server_target_fw_ver);
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

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

    // Sampai terbukti sebaliknya (handshake SKIP_BYTE, atau OTA selesai +
    // terverifikasi di bawah), anggap slave BELUM tentu pada versi yang
    // diharapkan. Ini menjaga wifiStreamTask tidak meneruskan perintah ke
    // slave selama proses cek/flash OTA sedang berlangsung.
    c3_programmer_set_version_match(false);

    // Reset variabel global header sebelum koneksi
    server_calculated_crc = 0;
    server_target_fw_ver = 0;

    // Cari IP server lewat discovery (lihat net_discovery.c). Kalau belum
    // pernah ketemu sama sekali sejak boot (mis. task ini jalan sebelum
    // discovery di main.cpp sempat sukses), coba discovery langsung di sini.
    const char *server_ip = net_discovery_get_server_ip();
    if (server_ip == NULL || server_ip[0] == '\0') {
        ESP_LOGW(TAG, "IP server belum diketahui, mencoba discovery...");
        if (net_discovery_find_server(NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Server tidak ditemukan, OTA slave dilewati kali ini.");
            vTaskDelete(NULL);
            return;
        }
        server_ip = net_discovery_get_server_ip();
    }

    char firmware_url[96];
    snprintf(firmware_url, sizeof(firmware_url), "http://%s:%d%s",
             server_ip, FIRMWARE_HTTP_PORT, FIRMWARE_HTTP_PATH);
    ESP_LOGI(TAG, "URL firmware slave (hasil discovery): %s", firmware_url);

    // 1. Inisialisasi HTTP Client dengan Event Handler
    esp_http_client_config_t http_cfg = {
        .url = firmware_url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .event_handler = _http_event_handler, // <-- Menangkap header secara otomatis
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
    
    // Ambil nilai dari variabel global yang sudah terisi lewat Event Handler
    uint32_t calculated_crc = server_calculated_crc;
    uint32_t target_fw_ver = (server_target_fw_ver != 0) ? server_target_fw_ver : FIRMWARE_VERSION;

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
            // Slave sendiri yang konfirmasi versinya sudah sama dengan target
            // -> aman untuk mulai meneruskan perintah/respon server ke slave.
            c3_programmer_set_version_match(true);
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
                    // Firmware baru sudah terverifikasi cocok dengan target versi
                    // yang dikirim di header. Slave akan reboot sesaat lagi;
                    // setelah ini aman meneruskan perintah/respon server ke slave.
                    c3_programmer_set_version_match(true);
                } else {
                    ESP_LOGE(TAG, "OTA TIDAK SELESAI: gagal mendapat konfirmasi dari slave.");
                    c3_programmer_set_version_match(false);
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