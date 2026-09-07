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

#define MASTER_UART_NUM       (UART_NUM_0)
#define MASTER_TX_PIN         (GPIO_NUM_1)  
#define MASTER_RX_PIN         (GPIO_NUM_3)  
#define CHUNK_SIZE            (512)
#define MAX_RETRIES           (5)

static const uint8_t SYNC_BYTES[6] = {0xC0, 0xFF, 0xFE, 0xAA, 0x55, 0x90};
#define ACK_BYTE              0x06
#define SKIP_BYTE             0x15   
#define OTA_DONE_BYTE         0x99   
#define FINAL_ACK_BYTE        0xA5   
#define OTA_ERROR_BYTE        0xE7   

#define LAST_CHUNK_TIMEOUT_MS (10000) // Diperbesar ke 10s untuk beri waktu Slave menulis flash & CRC
#define LAST_CHUNK_RETRIES    (5)    

extern const uint8_t slave_start[] asm("_binary_ESP_C3_Firmware_bin_start");
extern const uint8_t slave_end[]   asm("_binary_ESP_C3_Firmware_bin_end");

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
    // Memperbesar buffer RX Master untuk keamanan
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

    size_t slave_size = slave_end - slave_start;
    const uint8_t *firmware_ptr = slave_start;

    // Hitung CRC32 persis seperti cara Slave menghitung bertahap
    uint32_t calculated_crc = esp_crc32_le(0, firmware_ptr, slave_size);

    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, " Memulai Transmisi OTA Master");
    ESP_LOGI(TAG, " Ukuran Firmware : %u byte", (unsigned int)slave_size);
    ESP_LOGI(TAG, " Kalkulasi CRC32 : 0x%08X", (unsigned int)calculated_crc);
    ESP_LOGI(TAG, " Versi Firmware  : 0x%08X", (unsigned int)FIRMWARE_VERSION);
    ESP_LOGI(TAG, "==========================================");

    ota_header_t header;
    memcpy(header.sync, SYNC_BYTES, sizeof(SYNC_BYTES));
    header.fw_size    = (uint32_t)slave_size;
    header.fw_crc32   = calculated_crc;
    header.fw_version = FIRMWARE_VERSION;

    uint8_t rx_buf[1];
    bool synced = false;
    bool skip_update = false;

    // Flush HANYA SEBELUM HANDSHAKE
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
            vTaskDelete(NULL);
            return;
        } else {
            ESP_LOGW(TAG, "Timeout ACK dari Slave, mencoba ulang...");
            uart_flush_input(MASTER_UART_NUM);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (skip_update) {
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_sent = 0;
    int retry_count = 0;

    while (bytes_sent < slave_size) {
        size_t send_len = slave_size - bytes_sent;
        if (send_len > CHUNK_SIZE) {
            send_len = CHUNK_SIZE;
        }

        bool is_last_chunk = (bytes_sent + send_len >= slave_size);

        // JANGAN PANGGIL uart_flush_input DI SINI!
        uart_write_bytes(MASTER_UART_NUM, (const char *)(firmware_ptr + bytes_sent), send_len);
        uart_wait_tx_done(MASTER_UART_NUM, pdMS_TO_TICKS(500));

        int len = uart_read_bytes(MASTER_UART_NUM, rx_buf, 1, pdMS_TO_TICKS(5000));
        if (len > 0 && rx_buf[0] == ACK_BYTE) {
            bytes_sent += send_len;
            retry_count = 0;

            int progress = (bytes_sent * 100) / slave_size;
            ESP_LOGI(TAG, "Progres: %d%% (%u/%u byte)", progress, (unsigned int)bytes_sent, (unsigned int)slave_size);

            if (is_last_chunk) {
                ESP_LOGI(TAG, "Chunk terakhir diterima. Menunggu verifikasi & finalisasi OTA...");

                if (wait_for_ota_done_signal()) {
                    ESP_LOGI(TAG, "OTA SUKSES - Slave terkonfirmasi valid & akan reboot.");
                } else {
                    ESP_LOGE(TAG, "OTA TIDAK SELESAI: gagal mendapat konfirmasi dari slave.");
                }
            }
        } else if (len > 0 && master_check_error(rx_buf[0])) {
            ESP_LOGE(TAG, "OTA dibatalkan karena error di slave pada offset %u.", (unsigned int)bytes_sent);
            vTaskDelete(NULL);
            return;
        } else {
            retry_count++;
            ESP_LOGW(TAG, "Timeout ACK pada offset %u (Percobaan %d/%d)", (unsigned int)bytes_sent, retry_count, MAX_RETRIES);

            if (retry_count >= MAX_RETRIES) {
                ESP_LOGE(TAG, "Gagal Mengirim OTA: Slave tidak merespon!");
                vTaskDelete(NULL);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Task OTA Master selesai.");
    vTaskDelete(NULL);
}   