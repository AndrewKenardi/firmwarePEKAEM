#ifndef OTA_TASK_H
#define OTA_TASK_H

#include "esp_err.h"
#include <stddef.h>

#define UPDATE_CHECK_INTERVAL_MS (60 * 60 * 1000) // 1 jam kalau normal
#define UPDATE_RETRY_INTERVAL_MS (5 * 60 * 1000)   // 5 menit kalau ada kegagalan


/**
 * @brief Task tunggal yang menangani seluruh alur update:
 *        1. Cek & pasang firmware baru untuk ESP32-S sendiri (restart
 *           otomatis kalau ada update -- langkah 2 baru jalan siklus berikutnya).
 *        2. Cek & program ulang ESP32-C3 lewat UART, hanya kalau versinya beda.
 *
 *        Interval pengecekan otomatis lebih rapat (5 menit) kalau ada
 *        kegagalan di siklus sebelumnya, dan kembali normal (1 jam)
 *        begitu semua berjalan lancar.
 */
void vTaskUpdateManager(void *pvParameters);

/**
 * @brief Download isi file teks kecil (mis. berisi string versi "1.0.3")
 *        dari URL, dan simpan ke buffer. Cocok untuk file version.txt
 *        yang ukurannya cuma beberapa byte.
 */
esp_err_t version_check_fetch(const char *url, char *out_buf, size_t out_buf_size);

/**
 * @brief Konfirmasi firmware yang sedang berjalan valid, cegah rollback
 *        otomatis oleh bootloader. WAJIB dipanggil sekali di awal app_main().
 */
void ota_confirm_running_app(void);

/**
 * @brief Jalankan OTA sekali (blocking) dari URL yang diberikan. Kalau
 *        berhasil, device otomatis restart di dalam fungsi ini.
 *        Dipanggil oleh update_manager.c -- jangan buat task terpisah
 *        untuk ini lagi (vTaskOtaCheck sudah dihapus, fungsinya dilebur
 *        ke vTaskUpdateManager supaya self-OTA dan update C3 tidak
 *        berjalan sebagai dua mekanisme independen yang bisa tabrakan).
 */
esp_err_t start_ota_update(const char *url);

#endif // OTA_TASK_H