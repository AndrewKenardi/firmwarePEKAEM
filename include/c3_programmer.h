#ifndef C3_PROGRAMMER_H
#define C3_PROGRAMMER_H

#include "esp_err.h"
#include <stdbool.h>
#include "esp_system.h"
#include "driver/gpio.h" // Menambahkan definisi GPIO_NUM_X

#ifdef __cplusplus
extern "C" {
#endif

// CATATAN: MASTER_UART_NUM tetap di UART_NUM_0 (GPIO1/GPIO3) karena jalur
// fisik ke slave sudah di-solder/tidak bisa di-rewiring. Ini artinya UART
// ini TETAP berbagi kabel dengan console/log ESP-IDF & Arduino Serial --
// jadi noise/log APAPUN yang ikut lewat sini harus diasumsikan bisa sampai
// ke slave. Mitigasinya dipindah ke sisi PARSER di uartReceive.c (slave):
// hanya menerima baris yang PERSIS berformat "CMD:<token>\n" (lihat catatan
// di parse_command_string()), bukan dengan mengisolasi kabelnya.
#define MASTER_UART_NUM       (UART_NUM_0)
#define MASTER_TX_PIN         (GPIO_NUM_1)
#define MASTER_RX_PIN         (GPIO_NUM_3)
#define CHUNK_SIZE            (512)
#define MAX_RETRIES           (5)


static void master_uart_init(void);
void master_ota_task(void *pvParameters);

/**
 * @brief Di-set oleh master_ota_task() setelah proses flashing/verifikasi
 * firmware slave (chip C3) selesai dan versinya sudah dipastikan cocok
 * dengan yang diharapkan master.
 *
 * Task/modul lain (mis. wifiStreamTask) HARUS mengecek
 * c3_programmer_is_version_match() sebelum meneruskan perintah apa pun
 * ke slave lewat UART, supaya tidak ada perintah yang terkirim ke slave
 * yang masih menjalankan firmware lama / belum terverifikasi.
 */
void c3_programmer_set_version_match(bool match);

/**
 * @brief Mengecek apakah slave (chip C3) sudah terverifikasi berada pada
 * versi firmware yang sama dengan yang diharapkan master.
 * Thread-safe untuk dipanggil dari task manapun.
 */
bool c3_programmer_is_version_match(void);

#ifdef __cplusplus
}
#endif

#endif // C3_PROGRAMMER_H