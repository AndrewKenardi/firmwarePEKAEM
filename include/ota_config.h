#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

// ============================================================================
// SERVER -- ganti semua base URL di sini kalau server pindah, tidak perlu
// edit banyak file.
// ============================================================================
#define OTA_SERVER_BASE        "http://10.45.173.156:8000"

#define SELF_OTA_URL           OTA_SERVER_BASE "/firmwarePkm.bin"

#define C3_VERSION_URL         OTA_SERVER_BASE "/c3/c3_version.txt"
#define C3_BOOTLOADER_URL      OTA_SERVER_BASE "/c3/bootloader_c3.bin"
#define C3_PARTITION_URL       OTA_SERVER_BASE "/c3/partition-table_c3.bin"
#define C3_APP_URL             OTA_SERVER_BASE "/c3/ESP-C3_Firmware.bin"

// ============================================================================
// UART & pin kontrol ke ESP32-C3
// ============================================================================
#define C3_UART_PORT           UART_NUM_0
#define C3_UART_TX_PIN         1
#define C3_UART_RX_PIN         3
#define C3_BOOT_PIN            2   // -1 = board C3 pakai auto-reset circuit sendiri
#define C3_RESET_PIN           23   // -1 = board C3 pakai auto-reset circuit sendiri
#define C3_UART_BAUD_NORMAL    115200
#define C3_UART_BAUD_FLASHING  460800

// Offset flash C3 (bootloader C3 mulai di 0x0000, beda dari ESP32 klasik 0x1000)
#define C3_BOOTLOADER_OFFSET   0x0000
#define C3_PARTITION_OFFSET    0x8000
#define C3_APP_OFFSET          0x10000

// ============================================================================
// Interval & retry
// ============================================================================
#define UPDATE_CHECK_INTERVAL_MS   (60 * 60 * 1000)  // 1 jam kalau semua OK
#define UPDATE_RETRY_INTERVAL_MS   (5 * 60 * 1000)    // 5 menit kalau ada yang gagal
#define DOWNLOAD_CHUNK_SIZE        1024
#define HTTP_TIMEOUT_MS            15000
#define MAX_DOWNLOAD_RETRY         2  // percobaan ulang per-file kalau gagal di tengah jalan

// ============================================================================
// NVS
// ============================================================================
#define NVS_NAMESPACE_C3        "c3_ota"
#define NVS_KEY_C3_VERSION      "fw_version"
#define VERSION_BUF_LEN         32

#endif // OTA_CONFIG_H