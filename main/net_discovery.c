#include "net_discovery.h"
#include <string.h>
#include <errno.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "NET_DISCOVERY";

// Harus sama persis dengan yang dicek discovery_responder.py di server.
#define DISCOVERY_MAGIC        "PKM_DISCOVER_V1"
#define DISCOVERY_REPLY_PREFIX "PKM_HERE_V1"

#define DISCOVERY_RECV_TIMEOUT_MS  1000
#define DISCOVERY_MAX_ATTEMPTS     20   // ~20 detik total per pemanggilan

static char s_server_ip[NET_DISCOVERY_IP_STRLEN] = {0};

const char *net_discovery_get_server_ip(void)
{
    return s_server_ip;
}

// Hitung alamat broadcast dari IP+netmask STA yang sedang aktif. Lebih
// reliable daripada 255.255.255.255 polos di beberapa router/AP yang
// memfilter limited broadcast, tapi tetap fallback ke situ kalau IP STA
// belum siap.
static uint32_t compute_broadcast_addr(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            return (ip_info.ip.addr & ip_info.netmask.addr) | ~ip_info.netmask.addr;
        }
    }
    ESP_LOGW(TAG, "Gagal ambil IP/netmask STA, pakai broadcast 255.255.255.255");
    return 0xFFFFFFFF;
}

esp_err_t net_discovery_find_server(char *out_ip, size_t out_ip_len)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Gagal membuat socket discovery: errno %d", errno);
        return ESP_FAIL;
    }

    int broadcast_enable = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

    struct timeval tv = {
        .tv_sec  = DISCOVERY_RECV_TIMEOUT_MS / 1000,
        .tv_usec = (DISCOVERY_RECV_TIMEOUT_MS % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(NET_DISCOVERY_PORT);
    dest.sin_addr.s_addr = compute_broadcast_addr();

    bool found = false;

    for (int attempt = 1; attempt <= DISCOVERY_MAX_ATTEMPTS && !found; attempt++) {
        ESP_LOGI(TAG, "Mencari server di jaringan lokal... (%d/%d)", attempt, DISCOVERY_MAX_ATTEMPTS);

        int sent = sendto(sock, DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC), 0,
                          (struct sockaddr *)&dest, sizeof(dest));
        if (sent < 0) {
            ESP_LOGW(TAG, "sendto() broadcast gagal: errno %d", errno);
        }

        char rx_buf[64];
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&from_addr, &from_len);

        if (len > 0) {
            rx_buf[len] = '\0';
            if (strncmp(rx_buf, DISCOVERY_REPLY_PREFIX, strlen(DISCOVERY_REPLY_PREFIX)) == 0) {
                inet_ntoa_r(from_addr.sin_addr, s_server_ip, sizeof(s_server_ip));
                ESP_LOGI(TAG, "Server ditemukan di %s", s_server_ip);
                found = true;
                break;
            }
            // Bukan balasan yang kita kenal (mis. broadcast dari device lain
            // di jaringan yang sama) -- abaikan, coba lagi.
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }

    close(sock);

    if (!found) {
        ESP_LOGE(TAG, "Server tidak ditemukan setelah %d percobaan. "
                       "Pastikan discovery_responder.py jalan di server & "
                       "device/server ada di jaringan yang sama.",
                 DISCOVERY_MAX_ATTEMPTS);
        return ESP_FAIL;
    }

    if (out_ip != NULL && out_ip_len > 0) {
        strncpy(out_ip, s_server_ip, out_ip_len - 1);
        out_ip[out_ip_len - 1] = '\0';
    }
    return ESP_OK;
}