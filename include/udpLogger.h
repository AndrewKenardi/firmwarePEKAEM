#ifndef UDP_LOGGER_H
#define UDP_LOGGER_H

#include "esp_err.h"
#include <stdint.h>

typedef struct {
    const char *server_ip;      // IP tujuan, mis. "192.168.1.100"
    uint16_t    server_port;    // Port UDP tujuan, mis. 5005
    uint16_t    queue_len;      // Jumlah pesan log yang bisa dibuffer (default 32 jika 0)
    uint8_t     sender_priority;// Prioritas task pengirim UDP (default 3 jika 0, HARUS rendah)
} udp_logger_config_t;

/**
 * @brief Inisialisasi UDP logger. Setelah ini semua ESP_LOGx()
 *        akan tetap tampil di serial DAN dikirim ke server UDP.
 *        Panggil SETELAH WiFi/IP sudah terhubung (butuh network stack aktif).
 */
esp_err_t udp_logger_init(const udp_logger_config_t *config);

/**
 * @brief Kembalikan logging ke default (serial only) dan tutup socket.
 */
void udp_logger_deinit(void);

#endif // UDP_LOGGER_H