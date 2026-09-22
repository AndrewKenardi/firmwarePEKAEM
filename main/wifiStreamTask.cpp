/*
 * wifiStreamTask.cpp
 *
 * FIX: ml_safe_fb_return() sekarang mengambil camera_capture_mutex
 * (didefinisikan di cameraTask.c) sebelum memanggil esp_camera_fb_return().
 * Tanpa ini, esp_camera_fb_get() (dipanggil dari vTaskCameraRead di core lain)
 * dan esp_camera_fb_return() (dipanggil dari vTaskMLStream di sini) bisa
 * berjalan bersamaan dan merusak state internal pool frame buffer driver
 * esp32-camera, yang berujung pada timeout terus-menerus saat capture.
 */

#include "wifiStreamTask.h"
#include "cameraTask.h"
#include "esp_task_wdt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

// --- Arduino core & library WebSocket (C++) ---
#include <Arduino.h>
#include <WebSocketsClient_Generic.h>

extern "C" {
#include "uartTx.h"         // master_uart_send_cmd_with_ack()
#include "c3_programmer.h"  // c3_programmer_is_version_match()
}

extern EventGroupHandle_t wifiEventGroup;
extern QueueHandle_t frame_queue;
extern "C" esp_err_t connect_wifi(void);

// Timeout menunggu ACK dari slave saat meneruskan respon server (ms).
#define SLAVE_FORWARD_ACK_TIMEOUT_MS 500

// FIX: camera_capture_mutex sudah dideklarasikan sebagai extern di
// cameraTask.h (di-include di atas) dan didefinisikan di cameraTask.c.
// Mutex ini WAJIB dipakai setiap kali esp_camera_fb_get()/fb_return()
// dipanggil dari task manapun, agar tidak ada dua task yang menyentuh
// pool frame buffer driver kamera secara bersamaan. Tidak perlu
// dideklarasikan ulang di sini — cukup pakai langsung.

#ifndef IS_WIFI_CONNECTED_BIT
#define IS_WIFI_CONNECTED_BIT BIT0
#endif

static const char *TAG_WS = "WS_STREAM";

// ============================================================================
// KONFIGURASI SERVER WEBSOCKET (LIVE PRODUCTION)
// ============================================================================
#define WS_SERVER_HOST "socasob-ml.hallojanu.xyz"
#define WS_SERVER_PORT 443
#define WS_SERVER_PATH "/ws"
#define EYE_DISTANCE_THRESHOLD_MM 300 // Jarak <= 30cm dianggap "Dekat"

// Batas maksimum alokasi buffer pengiriman paket (100 KB cukup untuk frame JPEG)
#define MAX_TX_PACKET_SIZE (100 * 1024)

// Delay antar pengiriman frame (35 ms ≈ 25-28 FPS).
// Menaikkan angka ini ke 40-50 ms akan membuat koneksi jauh lebih stabil jika Wi-Fi lambat.
#define STREAM_FRAME_DELAY_MS 35

// Timeout maksimum menunggu mutex kamera saat akan me-return frame buffer.
// Diberi batas (bukan portMAX_DELAY) supaya task streaming tidak ikut macet
// selamanya kalau task lain menahan mutex lebih lama dari yang diharapkan.
#define CAMERA_MUTEX_WAIT_MS 200

// Ukuran buffer untuk payload mentah yang diteruskan ke slave. HARUS <= 26
// karena master_uart_send_cmd_with_ack() (uartTx.c) menyusunnya jadi
// "CMD:<payload>\n" di dalam buffer internal 32 byte miliknya sendiri.
#define CMD_BUF_FORWARD_SIZE 26

static WebSocketsClient s_ws;
static bool s_ws_started = false;
static bool s_ws_fully_connected = false; // Flag status SSL Handshake
static bool s_arduino_inited = false;

// Backoff otomatis untuk interval reconnect WSS. Handshake TLS yang gagal
// itu SENDIRI memblokir CPU cukup lama (matematika ECC, lihat catatan Task
// Watchdog di atas) -- kalau server sedang down/unreachable dalam waktu
// lama, retry tiap 3 detik terus-menerus berarti CPU dihajar berulang-ulang
// oleh percobaan handshake yang pasti gagal. Backoff ini memperlebar jarak
// antar percobaan kalau gagal beruntun, supaya frekuensi kejadian yang bisa
// memicu Task Watchdog jauh berkurang tanpa mengorbankan kecepatan
// reconnect saat server memang cuma putus sebentar.
#define RECONNECT_INTERVAL_BASE_MS   3000
#define RECONNECT_INTERVAL_MAX_MS    30000
#define RECONNECT_BACKOFF_THRESHOLD  3   // mulai backoff setelah gagal beruntun sebanyak ini
static uint32_t s_reconnect_interval_ms = RECONNECT_INTERVAL_BASE_MS;
static uint16_t s_consecutive_failures = 0;

// Mutex & Variabel Jarak Sensor VL53L0X
static SemaphoreHandle_t s_distance_mutex = NULL;
static uint16_t s_last_distance_mm = 0;
static bool s_last_distance_valid = false;

// Buffer reuse statis untuk transmisi data
static uint8_t *s_tx_packet_buffer = NULL;
extern "C" {
extern volatile uint32_t g_cam_stage, g_cam_iter, g_cam_fb_ok, g_cam_fb_null, g_cam_q_ok, g_cam_q_drop;
}

// ============================================================================
// HELPER: Kembalikan frame buffer kamera dengan aman
// ============================================================================
// FIX: Sekarang mengunci camera_capture_mutex sebelum memanggil
// esp_camera_fb_return(), supaya tidak bentrok dengan esp_camera_fb_get()
// yang dipanggil dari vTaskCameraRead (core/task berbeda).
static inline void ml_safe_fb_return(camera_fb_t *fb) {
    if (fb) esp_camera_fb_return(fb);
}

extern "C" void ml_stream_init(void) {
    if (s_distance_mutex == NULL) {
        s_distance_mutex = xSemaphoreCreateMutex();
    }

    // Alokasikan buffer pengiriman sekali saja di PSRAM jika ada, atau DRAM
    if (s_tx_packet_buffer == NULL) {
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32_SPIRAM_SUPPORT
        s_tx_packet_buffer = (uint8_t *) heap_caps_malloc(MAX_TX_PACKET_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_tx_packet_buffer) {
            ESP_LOGI(TAG_WS, "Buffer pengiriman WebSocket dialokasikan di PSRAM (%d bytes)", MAX_TX_PACKET_SIZE);
        }
#endif
        if (s_tx_packet_buffer == NULL) {
            s_tx_packet_buffer = (uint8_t *) malloc(MAX_TX_PACKET_SIZE);
            if (s_tx_packet_buffer) {
                ESP_LOGI(TAG_WS, "Buffer pengiriman WebSocket dialokasikan di Internal DRAM (%d bytes)", MAX_TX_PACKET_SIZE);
            } else {
                ESP_LOGE(TAG_WS, "FATAL: Gagal mengalokasikan memori buffer pengiriman WebSocket!");
            }
        }
    }
}

extern "C" void ml_stream_set_distance(uint16_t distance_mm, bool valid) {
    if (s_distance_mutex == NULL) {
        ml_stream_init();
    }
    if (s_distance_mutex != NULL && xSemaphoreTake(s_distance_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_last_distance_mm = distance_mm;
        s_last_distance_valid = valid;
        xSemaphoreGive(s_distance_mutex);
    }
}

static void ml_get_distance(uint16_t *distance_mm, bool *valid) {
    *distance_mm = 0;
    *valid = false;
    if (s_distance_mutex == NULL) return;
    if (xSemaphoreTake(s_distance_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *distance_mm = s_last_distance_mm;
        *valid = s_last_distance_valid;
        xSemaphoreGive(s_distance_mutex);
    }
}

// Dipanggil tiap kali WStype_DISCONNECTED / WStype_ERROR. Setelah gagal
// beruntun melewati RECONNECT_BACKOFF_THRESHOLD, jarak antar percobaan
// reconnect dilipatgandakan (dibatasi RECONNECT_INTERVAL_MAX_MS) supaya
// tidak menghajar CPU dengan percobaan handshake TLS yang pasti gagal terus.
static void handle_reconnect_backoff(void) {
    if (s_consecutive_failures < UINT16_MAX) {
        s_consecutive_failures++;
    }

    if (s_consecutive_failures < RECONNECT_BACKOFF_THRESHOLD) {
        return; // belum cukup gagal beruntun, biarkan interval normal
    }

    uint32_t shift = s_consecutive_failures - RECONNECT_BACKOFF_THRESHOLD + 1;
    if (shift > 10) shift = 10; // cegah overflow shift, plafon sudah kena duluan lewat MAX_MS
    uint32_t candidate = RECONNECT_INTERVAL_BASE_MS << shift;
    uint32_t new_interval = (candidate > RECONNECT_INTERVAL_MAX_MS || candidate < RECONNECT_INTERVAL_BASE_MS)
                                 ? RECONNECT_INTERVAL_MAX_MS
                                 : candidate;

    if (new_interval != s_reconnect_interval_ms) {
        s_reconnect_interval_ms = new_interval;
        s_ws.setReconnectInterval(s_reconnect_interval_ms);
        ESP_LOGW(TAG_WS, "[WS] Gagal %u kali beruntun -> interval reconnect diperlebar jadi %lu ms.",
                 (unsigned)s_consecutive_failures, (unsigned long)s_reconnect_interval_ms);
    }
}

// ============================================================================
// WEBSOCKET EVENT HANDLER
// ============================================================================
static void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            s_ws_fully_connected = false;
            ESP_LOGW(TAG_WS, "[WS] Terputus dari ML Server.");
            handle_reconnect_backoff();
            break;

        case WStype_CONNECTED:
            s_ws_fully_connected = true;
            ESP_LOGI(TAG_WS, "[WS] BERHASIL TERHUBUNG DENGAN SSL! Host: %s", WS_SERVER_HOST);
            // Konek sukses -> reset backoff, balik ke interval normal.
            s_consecutive_failures = 0;
            if (s_reconnect_interval_ms != RECONNECT_INTERVAL_BASE_MS) {
                s_reconnect_interval_ms = RECONNECT_INTERVAL_BASE_MS;
                s_ws.setReconnectInterval(s_reconnect_interval_ms);
                ESP_LOGI(TAG_WS, "[WS] Backoff direset ke %lu ms.", (unsigned long)s_reconnect_interval_ms);
            }
            break;

        case WStype_TEXT: {
            ESP_LOGI(TAG_WS, "[WS] Server balas: %.*s", (int)length, payload);

            // Teruskan respon server ke slave (chip C3) lewat UART, supaya
            // slave mengganti layar (wajah) dan suara sesuai respon server.
            // HANYA dilakukan jika slave sudah terverifikasi berada pada
            // versi firmware yang sama (lihat c3_programmer_set_version_match(),
            // dipanggil dari master_ota_task setelah OTA+verifikasi selesai).
            if (!c3_programmer_is_version_match()) {
                ESP_LOGW(TAG_WS,
                         "[WS->UART] Respon server TIDAK diteruskan ke slave: "
                         "versi firmware slave belum terverifikasi sama.");
                break;
            }

            // PENTING: master_uart_send_cmd_with_ack() (uartTx.c) SUDAH
            // menambahkan prefix "CMD:" dan "\n" sendiri, dan buffer
            // internalnya cuma 32 byte ("CMD:" + isi + "\n" + '\0').
            // Jadi di sini kita kirim payload MENTAH saja (tanpa prefix),
            // dan dipotong supaya muat.
            char cmd_buf[CMD_BUF_FORWARD_SIZE];
            size_t copy_len = length;
            if (copy_len > sizeof(cmd_buf) - 1) {
                copy_len = sizeof(cmd_buf) - 1;
            }
            memcpy(cmd_buf, payload, copy_len);
            cmd_buf[copy_len] = '\0';

            // Server bisa membalas berkali-kali per detik (mis. hasil per
            // frame kamera) walau isinya belum berubah. Kalau isinya SAMA
            // dengan yang terakhir kali berhasil diteruskan, jangan kirim
            // ulang -- supaya slave tidak menerima command yang sama terus-
            // menerus (yang bikin suara terkesan "restart" berulang-ulang).
            static char s_last_forwarded[CMD_BUF_FORWARD_SIZE] = {0};
            if (strcmp(cmd_buf, s_last_forwarded) == 0) {
                break;
            }

            bool forwarded = master_uart_send_cmd_with_ack(
                cmd_buf, SLAVE_FORWARD_ACK_TIMEOUT_MS, MAX_RETRIES);

            if (forwarded) {
                ESP_LOGI(TAG_WS, "[WS->UART] Respon server diteruskan ke slave: %s", cmd_buf);
                strcpy(s_last_forwarded, cmd_buf);
            } else {
                ESP_LOGE(TAG_WS,
                         "[WS->UART] Gagal meneruskan respon ke slave setelah %d percobaan: %s",
                         MAX_RETRIES, cmd_buf);
                // TIDAK di-update s_last_forwarded, supaya percobaan
                // berikutnya (walau isinya sama) tetap dicoba kirim ulang.
            }
            break;
        }

        case WStype_ERROR:
            s_ws_fully_connected = false;
            ESP_LOGE(TAG_WS, "[WS] Error event WebSocket");
            handle_reconnect_backoff();
            break;

        default:
            break;
    }
}

static void ml_init_websocket(void) {
    if (s_ws_started) {
        return;
    }

    s_ws.beginSSL(WS_SERVER_HOST, WS_SERVER_PORT, WS_SERVER_PATH);
    s_ws.onEvent(webSocketEvent);
    s_ws.setReconnectInterval(3000);

    s_ws_started = true;
}

// ============================================================================
// TASK UTAMA STREAMING FRAME WEBSOCKET
// ============================================================================
extern "C" void vTaskMLStream(void *pvParameters) {
    ESP_LOGI(TAG_WS, "=== TASK ML STREAM DIPANGGIL DAN MULAI BERJALAN ===");
    esp_task_wdt_add(NULL);

    // PASTIKAN robot_id ini sudah terdaftar di database Backend
    const char *robot_id = "dummyrobot01";
    uint8_t robot_id_len = (uint8_t)strlen(robot_id);

    ml_stream_init();

    // Tunggu Wi-Fi terhubung
    xEventGroupWaitBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (!s_arduino_inited) {
        initArduino();
        s_arduino_inited = true;
    }

    ESP_LOGI(TAG_WS, "Wi-Fi Terverifikasi Konek, Menginisialisasi Client WebSocket...");
    ml_init_websocket();

    // ---- variabel statistik ----
    uint32_t stat_sent = 0;
    uint32_t dbg_bytes = 0;
    uint32_t dbg_fail = 0;
    int64_t  max_send_us = 0;
    int64_t  total_send_us = 0;
    int64_t  stat_t0 = esp_timer_get_time();
    wifi_ap_record_t ap;

    // TES SEMENTARA: pura-pura ada objek 10 cm supaya is_dekat = 1.
    // Hapus tanda komentar untuk mengetes reaksi server, lalu hapus lagi setelahnya.
    // ml_stream_set_distance(100, true);

    for (;;) {
        // Proses polling internal library WebSocket (Ping/Pong, Reconnect, RX)
        esp_task_wdt_reset();
        s_ws.loop();

        // Log statistik tiap 2 detik
        if (esp_timer_get_time() - stat_t0 >= 2000000) {
            int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
            unsigned avg_ms = stat_sent ? (unsigned)((total_send_us / stat_sent) / 1000) : 0;
            unsigned avg_kb = stat_sent ? (unsigned)((dbg_bytes / stat_sent) / 1024) : 0;

            ESP_LOGI(TAG_WS,
                     "WS: terkirim=%u gagal=%u bytes=%u (~%uKB/frame) send_avg=%ums send_max=%ums rssi=%ddBm conn=%d",
                     (unsigned)stat_sent, (unsigned)dbg_fail, (unsigned)dbg_bytes, avg_kb,
                     avg_ms, (unsigned)(max_send_us / 1000), rssi, (int)s_ws.isConnected());

            stat_sent = 0;
            dbg_bytes = 0;
            dbg_fail = 0;
            max_send_us = 0;
            total_send_us = 0;
            stat_t0 = esp_timer_get_time();
        }

        // 1. JIKA WEBSOCKET BELUM KONEK: DRAIN QUEUE & BERI JEDA UNTUK SSL HANDSHAKE
        if (!s_ws_fully_connected) {
            camera_fb_t *dummy_fb = NULL;
            while (xQueueReceive(frame_queue, &dummy_fb, 0) == pdTRUE) {
                ml_safe_fb_return(dummy_fb);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 2. AMBIL FRAME DARI QUEUE
        camera_fb_t *fb = NULL;
        if (xQueueReceive(frame_queue, &fb, pdMS_TO_TICKS(20)) == pdTRUE && fb != NULL) {

            const size_t jpeg_len = fb->len;
            const size_t required_packet_size = 1 + robot_id_len + 1 + jpeg_len;

            if (s_tx_packet_buffer != NULL && required_packet_size <= MAX_TX_PACKET_SIZE) {

                uint16_t distance_mm = 0;
                bool distance_valid = false;
                ml_get_distance(&distance_mm, &distance_valid);
                uint8_t is_dekat = (distance_valid && distance_mm <= EYE_DISTANCE_THRESHOLD_MM) ? 1 : 0;
                // uint8_t is_dekat = true;

                // Susun Payload Biner: [Len ID][ID Robot][Is Dekat][Data JPEG]
                s_tx_packet_buffer[0] = robot_id_len;
                memcpy(s_tx_packet_buffer + 1, robot_id, robot_id_len);
                s_tx_packet_buffer[1 + robot_id_len] = is_dekat;
                memcpy(s_tx_packet_buffer + 1 + robot_id_len + 1, fb->buf, jpeg_len);

                // Data sudah tersalin, lepas frame buffer kamera sekarang
                ml_safe_fb_return(fb);
                fb = NULL;

                // Ukur berapa lama sendBIN() memblokir
                int64_t t_send0 = esp_timer_get_time();
                static bool jpeg_checked = false;
                if (!jpeg_checked && jpeg_len > 4) {
                    jpeg_checked = true;
                    const uint8_t *j = s_tx_packet_buffer + 1 + robot_id_len + 1;
                    ESP_LOGI(TAG_WS, "JPEG cek: len=%u head=%02X%02X (harus FFD8) tail=%02X%02X (harus FFD9)",
                            (unsigned)jpeg_len, j[0], j[1], j[jpeg_len - 2], j[jpeg_len - 1]);
                }
                bool sent = s_ws.sendBIN(s_tx_packet_buffer, required_packet_size);
                int64_t send_us = esp_timer_get_time() - t_send0;

                if (!sent) {
                    dbg_fail++;
                    ESP_LOGE(TAG_WS, "Gagal kirim, size=%u", (unsigned)required_packet_size);
                } else {
                    stat_sent++;
                    dbg_bytes += required_packet_size;
                    total_send_us += send_us;
                    if (send_us > max_send_us) max_send_us = send_us;

                    //Testing
                    // vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_DELAY_MS));

                    taskYIELD();
                }
            } else {
                ESP_LOGE(TAG_WS, "Ukuran paket (%d bytes) melebihi batas buffer!", (int)required_packet_size);
                ml_safe_fb_return(fb);
                fb = NULL;
            }
        } else {
            // Queue kosong, beri CPU ke task lain
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}
extern "C" void vTaskWifiConnect(void *pvParameter) {
    while (connect_wifi() != ESP_OK) {
        ESP_LOGW("WIFI", "Wi-Fi belum terhubung, mencoba ulang...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("WIFI", "Wi-Fi Berhasil Terhubung!");
    xEventGroupSetBits(wifiEventGroup, IS_WIFI_CONNECTED_BIT);

    // Matikan mode hemat daya Wi-Fi untuk stabilitas streaming latency rendah
    esp_wifi_set_ps(WIFI_PS_NONE);
    vTaskDelete(NULL);
}