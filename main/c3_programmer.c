#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_crc.h"
#include "fw_version.h"

static const char *TAG = "MASTER_OTA";

// Konfiguras   i Hardware UART Master
#define MASTER_UART_NUM       (UART_NUM_0)
#define MASTER_TX_PIN         (GPIO_NUM_1)  // Hubungkan ke RX Slave (GPIO 0)
#define MASTER_RX_PIN         (GPIO_NUM_3)  // Hubungkan ke TX Slave (GPIO 1)
#define CHUNK_SIZE            (512)
#define MAX_RETRIES           (5)

// Konstanta Protokol
static const uint8_t SYNC_BYTES[6] = {0xC0, 0xFF, 0xFE, 0xAA, 0x55, 0x90};
#define ACK_BYTE              0x06
#define SKIP_BYTE             0x15   // Slave balas ini kalau versi sudah sama, tidak perlu update

// Embedded Firmware Binary
extern const uint8_t slave_start[] asm("_binary_ESP_C3_Firmware_bin_start");
extern const uint8_t slave_end[]   asm("_binary_ESP_C3_Firmware_bin_end");

// Struktur Header OTA (18 Byte Total: 6 sync + 4 size + 4 crc + 4 version)
#pragma pack(push, 1)
typedef struct {
    uint8_t  sync[6];     // 6 Byte Sync Protocol
    uint32_t fw_size;     // Total ukuran biner firmware (Byte)
    uint32_t fw_crc32;    // Target CRC32 firmware
    uint32_t fw_version;  // Versi firmware yang akan di-flash
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
    ESP_ERROR_CHECK(uart_driver_install(MASTER_UART_NUM, 1024, 1024, 0, NULL, 0));
}

void master_ota_task(void *pvParameters)
{
    master_uart_init();

    // 1. Hitung Ukuran & CRC32 Firmware
    size_t slave_size = slave_end - slave_start;
    const uint8_t *firmware_ptr = slave_start;
    uint32_t calculated_crc = esp_crc32_le(0, firmware_ptr, slave_size);

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " Memulai Transmisi OTA Master");
    ESP_LOGI(TAG, " Ukuran Firmware : %u byte", (unsigned int)slave_size);
    ESP_LOGI(TAG, " Kalkulasi CRC32 : 0x%08X", (unsigned int)calculated_crc);
    ESP_LOGI(TAG, " Versi Firmware  : 0x%08X", (unsigned int)FIRMWARE_VERSION);
    ESP_LOGI(TAG, "==========================================");

    // 2. Siapkan Paket Header
    ota_header_t header;
    memcpy(header.sync, SYNC_BYTES, sizeof(SYNC_BYTES));
    header.fw_size    = (uint32_t)slave_size;
    header.fw_crc32   = calculated_crc;
    header.fw_version = FIRMWARE_VERSION;

    // 3. Loop Handshake Tangguh
    uint8_t rx_buf[1];
    bool synced = false;
    bool skip_update = false;

    while (!synced) {
        // Bersihkan buffer lokal sebelum kirim
        uart_flush_input(MASTER_UART_NUM);

        ESP_LOGI(TAG, "Mengirim Sync Header ke Slave...");
        uart_write_bytes(MASTER_UART_NUM, (const char *)&header, sizeof(ota_header_t));

        // Tunggu respons dari Slave. Bisa ACK (lanjut update) atau SKIP (versi sudah sama)
        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(10000));

        if (len > 0 && rx_buf[0] == ACK_BYTE) {
            synced = true;
            ESP_LOGI(TAG, "Handshake Sukses! Slave siap menerima chunk.");
            vTaskDelay(pdMS_TO_TICKS(200)); // Jeda krusial agar Slave masuk loop data
        } else if (len > 0 && rx_buf[0] == SKIP_BYTE) {
            ESP_LOGI(TAG, "==========================================");
            ESP_LOGI(TAG, " Versi Slave sudah sama (0x%08X). OTA dilewati.", (unsigned int)FIRMWARE_VERSION);
            ESP_LOGI(TAG, "==========================================");
            skip_update = true;
            synced = true; // Keluar dari loop handshake tanpa error
        } else {
            ESP_LOGW(TAG, "Timeout ACK dari Slave, mencoba ulang dalam 1.5 detik...");
            vTaskDelay(pdMS_TO_TICKS(1500));
        }
    }

    // Kalau Slave bilang versi sudah sama, langsung selesai tanpa kirim data
    if (skip_update) {
        vTaskDelete(NULL);
        return;
    }

    // 4. Kirim Data Firmware Chunk demi Chunk
    size_t bytes_sent = 0;
    int retry_count = 0;

    while (bytes_sent < slave_size) {
        size_t send_len = slave_size - bytes_sent;
        if (send_len > CHUNK_SIZE) {
            send_len = CHUNK_SIZE;
        }

        uart_flush_input(MASTER_UART_NUM);
        uart_write_bytes(MASTER_UART_NUM, (const char *)(firmware_ptr + bytes_sent), send_len);

        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(3000));
        if (len > 0 && rx_buf[0] == ACK_BYTE) {
            bytes_sent += send_len;
            retry_count = 0;

            int progress = (bytes_sent * 100) / slave_size;
            ESP_LOGI(TAG, "Progres: %d%% (%u/%u byte)", progress, (unsigned int)bytes_sent, (unsigned int)slave_size);
        } else {
            retry_count++;
            ESP_LOGW(TAG, "Timeout ACK pada offset %u (Percobaan %d/%d)",
                     (unsigned int)bytes_sent, retry_count, MAX_RETRIES);

            if (retry_count >= MAX_RETRIES) {
                ESP_LOGE(TAG, "Gagal Mengirim OTA: Slave tidak merespon / Terputus!");
                vTaskDelete(NULL);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Pengiriman Firmware Selesai Total!");
    ESP_LOGI(TAG, "Membiarkan Slave verifikasi CRC32 dan Reboot...");

    vTaskDelete(NULL);
}