#ifndef UPDATE_MANAGER_H
#define UPDATE_MANAGER_H

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

#endif // UPDATE_MANAGER_H