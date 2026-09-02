#include "udpLogger.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG_UDPLOG = "UDP_LOGGER";

#define UDP_LOG_MSG_SIZE      256   // panjang maksimal 1 baris log yang di-buffer
#define UDP_LOG_DEFAULT_QLEN  32
#define UDP_LOG_DEFAULT_PRIO  3

static int                udp_sock  = -1;
static struct sockaddr_in dest_addr;
static QueueHandle_t      log_queue = NULL;
static TaskHandle_t       sender_task_handle = NULL;
static vprintf_like_t     original_vprintf = NULL;
static volatile bool      logger_running = false;

typedef struct {
    char msg[UDP_LOG_MSG_SIZE];
    uint16_t len;
} udp_log_msg_t;

// Task terpisah, prioritas rendah -> tidak mengganggu task real-time (kamera dll).
// Task ini yang benar-benar melakukan sendto() secara blocking, jauh dari
// task pemanggil ESP_LOGx().
static void udp_log_sender_task(void *pvParameters)
{
    udp_log_msg_t item;

    while (logger_running) {
        if (xQueueReceive(log_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (udp_sock >= 0) {
                sendto(udp_sock, item.msg, item.len, 0,
                       (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            }
        }
    }

    vTaskDelete(NULL);
}

// Hook pengganti vprintf bawaan esp_log.
// PENTING: fungsi ini HARUS cepat & tidak boleh blocking lama, karena
// dipanggil langsung dari task manapun yang sedang logging (termasuk task
// prioritas tinggi). Makanya di sini cuma vsnprintf + xQueueSend non-blocking
// (timeout 0) -- kalau queue penuh, pesan langsung di-drop, bukan menunggu.
static int udp_logger_vprintf(const char *fmt, va_list args)
{
    char buf[UDP_LOG_MSG_SIZE];

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(buf, sizeof(buf), fmt, args_copy);
    va_end(args_copy);

    // Tetap tampilkan ke serial console seperti biasa (kalau ada monitor)
    int ret = len;
    if (original_vprintf) {
        ret = original_vprintf(fmt, args);
    }

    if (log_queue != NULL && len > 0) {
        udp_log_msg_t item;
        item.len = (len < (int)sizeof(item.msg)) ? (uint16_t)len : (uint16_t)(sizeof(item.msg) - 1);
        memcpy(item.msg, buf, item.len);

        // timeout 0 -> TIDAK PERNAH block task pemanggil.
        // Kalau queue penuh (sender kewalahan / network lambat),
        // pesan ini di-drop, bukan menahan task lain.
        xQueueSend(log_queue, &item, 0);
    }

    return len;
}

esp_err_t udp_logger_init(const udp_logger_config_t *config)
{
    if (config == NULL || config->server_ip == NULL || config->server_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (udp_sock >= 0) {
        ESP_LOGW(TAG_UDPLOG, "UDP logger sudah aktif");
        return ESP_OK;
    }

    uint16_t qlen = config->queue_len ? config->queue_len : UDP_LOG_DEFAULT_QLEN;
    uint8_t  prio = config->sender_priority ? config->sender_priority : UDP_LOG_DEFAULT_PRIO;

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG_UDPLOG, "Gagal membuat socket UDP: errno %d", errno);
        return ESP_FAIL;
    }

    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family      = AF_INET;
    dest_addr.sin_port        = htons(config->server_port);
    dest_addr.sin_addr.s_addr = inet_addr(config->server_ip);

    // Non-blocking send di level socket juga, sebagai lapisan aman tambahan
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(udp_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    log_queue = xQueueCreate(qlen, sizeof(udp_log_msg_t));
    if (log_queue == NULL) {
        close(udp_sock);
        udp_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    logger_running = true;

    // Sender task: prioritas RENDAH (default 3) supaya task kamera/WiFi/OTA
    // (prioritas 15-20) selalu menang scheduling dibanding pengiriman log.
    // Ditaruh di core 0 (APP_CPU) secara default lewat xTaskCreate biasa,
    // supaya tidak berebut waktu dengan task kamera yang di-pin ke core 1.
    BaseType_t task_ret = xTaskCreate(
        udp_log_sender_task, "udpLogSender", 3072, NULL, prio, &sender_task_handle);

    if (task_ret != pdPASS) {
        vQueueDelete(log_queue);
        log_queue = NULL;
        close(udp_sock);
        udp_sock = -1;
        logger_running = false;
        return ESP_ERR_NO_MEM;
    }

    original_vprintf = esp_log_set_vprintf(udp_logger_vprintf);

    ESP_LOGI(TAG_UDPLOG, "UDP logger aktif (async) -> %s:%d, queue=%d, prio=%d",
             config->server_ip, config->server_port, qlen, prio);

    return ESP_OK;
}

void udp_logger_deinit(void)
{
    if (original_vprintf) {
        esp_log_set_vprintf(original_vprintf);
        original_vprintf = NULL;
    }

    logger_running = false;

    if (log_queue) {
        // Bangunkan sender task supaya bisa keluar dari portMAX_DELAY dan self-delete
        udp_log_msg_t dummy = {0};
        xQueueSend(log_queue, &dummy, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        vQueueDelete(log_queue);
        log_queue = NULL;
    }

    if (udp_sock >= 0) {
        close(udp_sock);
        udp_sock = -1;
    }
}