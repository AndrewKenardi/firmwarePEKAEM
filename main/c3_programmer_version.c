/*
 * c3_programmer_version.c
 *
 * Menyimpan status "apakah slave (chip C3) sudah berada pada versi firmware
 * yang sama dengan yang diharapkan master". Dipakai sebagai gerbang sebelum
 * meneruskan perintah apa pun ke slave lewat UART (lihat wifiStreamTask.cpp).
 *
 * Set c3_programmer_set_version_match(true) di dalam master_ota_task() Anda
 * yang sudah ada, tepat setelah proses flashing + verifikasi versi slave
 * selesai dan cocok. Set kembali ke false di awal proses OTA / jika versi
 * tidak cocok, supaya perintah tidak diteruskan ke slave yang sedang
 * di-flash ulang atau masih menjalankan firmware lama.
 */

#include "c3_programmer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static volatile bool s_slave_version_match = false;
static SemaphoreHandle_t s_version_match_mutex = NULL;

static SemaphoreHandle_t get_mutex(void)
{
    if (s_version_match_mutex == NULL) {
        s_version_match_mutex = xSemaphoreCreateMutex();
    }
    return s_version_match_mutex;
}

void c3_programmer_set_version_match(bool match)
{
    SemaphoreHandle_t mutex = get_mutex();
    if (mutex != NULL && xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_slave_version_match = match;
        xSemaphoreGive(mutex);
    } else {
        // Gagal ambil mutex (jarang terjadi): tetap set langsung, bool
        // adalah operasi atomik pada ESP32 sehingga tetap aman dibaca.
        s_slave_version_match = match;
    }
}

bool c3_programmer_is_version_match(void)
{
    return s_slave_version_match;
}