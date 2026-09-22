#ifndef NET_DISCOVERY_H
#define NET_DISCOVERY_H

#include "esp_err.h"
#include <stddef.h>

// "255.255.255.255" + null
#define NET_DISCOVERY_IP_STRLEN  16

// Port UDP yang dipakai untuk broadcast "siapa server-nya" ke jaringan
// lokal. Harus SAMA PERSIS dengan port yang didengarkan
// discovery_responder.py di sisi server (lihat file companion).
#define NET_DISCOVERY_PORT       50000

/**
 * @brief Broadcast ke jaringan lokal (subnet Wi-Fi yang sedang aktif)
 * mencari server, dan MENUNGGU (blocking) sampai server membalas atau
 * percobaan habis.
 *
 * Cara kerja: kirim paket UDP berisi magic string ke alamat broadcast
 * subnet saat ini; discovery_responder.py di server membalas dengan magic
 * string lain; IP pengirim balasan itulah yang dipakai sebagai IP server.
 * Ini otomatis benar walau IP hotspot/DHCP-nya beda-beda, selama ESP32 dan
 * server ada di jaringan (subnet) yang sama.
 *
 * @param out_ip      Buffer output (boleh NULL kalau tidak perlu, cukup
 *                     pakai net_discovery_get_server_ip() nanti).
 * @param out_ip_len  Ukuran buffer out_ip (minimal NET_DISCOVERY_IP_STRLEN).
 * @return ESP_OK kalau server ketemu, ESP_FAIL kalau tidak ketemu sampai
 *         retry habis (silakan panggil ulang fungsi ini nanti).
 */
esp_err_t net_discovery_find_server(char *out_ip, size_t out_ip_len);

/**
 * @brief IP server hasil discovery TERAKHIR yang berhasil. String kosong
 * ("") kalau belum pernah berhasil sama sekali.
 */
const char *net_discovery_get_server_ip(void);

#endif // NET_DISCOVERY_H