#include "connect_wifi.h"
#include <string.h>

int wifi_connect_status = 0;
static const char *WIFI_TAG = "Connect_WiFi";

// ============================================================================
// DAFTAR HOTSPOT YANG BOLEH DICOBA, URUT PRIORITAS.
// Tambah/ubah/hapus baris di sini kalau ada hotspot baru -- tidak perlu ubah
// logic di bawah sama sekali.
// ============================================================================
typedef struct {
    const char *ssid;
    const char *password;
} wifi_candidate_t;

static const wifi_candidate_t s_wifi_candidates[] = {
    { "TRIKBB",       "pemadamkebakaran" },
    { "AIR",   "Haraptenang2212" },
    { "yoyoyo", "qwertyui"}   // TODO: ganti sesuai hotspot cadangan Anda
    // { "HotspotTiga", "passwordTiga" },           // contoh nambah kandidat ke-3
};
#define WIFI_CANDIDATE_COUNT (sizeof(s_wifi_candidates) / sizeof(s_wifi_candidates[0]))

// Berapa kali retry SSID yang SAMA sebelum pindah ke kandidat berikutnya.
// Dibuat kecil (bukan puluhan) supaya kalau hotspot #1 memang mati, device
// tidak lama-lama "ngotot" sebelum coba hotspot #2.
#define RETRY_PER_CANDIDATE  3

#define WIFI_CONNECTED_BIT BIT0

// Event group ini SENGAJA dibuat sekali & TIDAK PERNAH di-delete (beda dari
// versi sebelumnya yang men-delete-nya di akhir connect_wifi()). Kenapa:
// event_handler() tetap terpasang selamanya dan akan terus menerima event
// WIFI_EVENT_STA_DISCONNECTED bahkan lama setelah connect_wifi() pertama
// kali return -- kalau event group-nya sudah di-delete, xEventGroupSetBits()
// di situ jadi use-after-free. Dengan dibuat persisten, aman dipanggil kapan
// saja, dan otomatis mendukung "kalau hotspot lagi jalan mati, pindah ke
// hotspot lain" bahkan di tengah operasi (bukan cuma waktu boot).
static EventGroupHandle_t s_wifi_event_group = NULL;
static size_t s_candidate_idx = 0;
static int    s_retry_num = 0;
static bool   s_wifi_stack_initialized = false;

static void apply_candidate_and_connect(size_t idx)
{
    const wifi_candidate_t *c = &s_wifi_candidates[idx % WIFI_CANDIDATE_COUNT];
    ESP_LOGI(WIFI_TAG, "Mencoba SSID #%u/%u: \"%s\"",
             (unsigned)(idx % WIFI_CANDIDATE_COUNT) + 1, (unsigned)WIFI_CANDIDATE_COUNT, c->ssid);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, c->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, c->password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    s_retry_num = 0;
    esp_wifi_connect();
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        apply_candidate_and_connect(s_candidate_idx);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_connect_status = 0;

        s_retry_num++;
        if (s_retry_num < RETRY_PER_CANDIDATE) {
            ESP_LOGW(WIFI_TAG, "Gagal konek ke SSID \"%s\", retry (%d/%d)...",
                     s_wifi_candidates[s_candidate_idx % WIFI_CANDIDATE_COUNT].ssid,
                     s_retry_num, RETRY_PER_CANDIDATE);
            esp_wifi_connect();
        } else {
            // Sudah dicoba beberapa kali di kandidat ini & tetap gagal ->
            // pindah ke kandidat berikutnya. TIDAK PERNAH benar-benar
            // menyerah: kalau semua kandidat habis dicoba, ulang lagi dari
            // kandidat #0 (siapa tahu hotspot pertama sudah nyala lagi).
            s_candidate_idx = (s_candidate_idx + 1) % WIFI_CANDIDATE_COUNT;
            ESP_LOGW(WIFI_TAG, "Pindah coba SSID berikutnya...");
            apply_candidate_and_connect(s_candidate_idx);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "Terhubung ke \"%s\", got ip:" IPSTR,
                 s_wifi_candidates[s_candidate_idx % WIFI_CANDIDATE_COUNT].ssid,
                 IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        wifi_connect_status = 1;
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t connect_wifi(void)
{
    esp_err_t err;

    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
    } else {
        // Panggilan ulang (mis. dari loop retry pemanggil) -- bersihkan
        // bit lama sebelum menunggu lagi, supaya tidak langsung "lolos"
        // dari sisa bit basi sebelumnya.
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }

    // --- Idempotent init guards ---
    // esp_netif_init() / esp_event_loop_create_default() must only run once
    // for the lifetime of the program.
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    if (!s_wifi_stack_initialized) {
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                            IP_EVENT_STA_GOT_IP,
                                                            &event_handler,
                                                            NULL,
                                                            &instance_got_ip));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        // esp_wifi_start() memicu WIFI_EVENT_STA_START -> event_handler()
        // otomatis memanggil apply_candidate_and_connect() dari kandidat #0.
        ESP_ERROR_CHECK(esp_wifi_start());

        s_wifi_stack_initialized = true;
    } else {
        // Wifi stack sudah jalan -- minta konek ulang mulai dari kandidat
        // yang lagi aktif (event_handler yang akan lanjut cycling kalau gagal).
        apply_candidate_and_connect(s_candidate_idx);
    }

    ESP_LOGI(WIFI_TAG, "Menunggu koneksi Wi-Fi (%u kandidat SSID terdaftar)...",
             (unsigned)WIFI_CANDIDATE_COUNT);

    // Nonaktifkan modem sleep / power save. Power save bisa bikin lonjakan
    // latensi beberapa detik saat radio "bangun" untuk kirim/terima paket.
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(80); // default ~80 (20dBm)

    // Karena event_handler() di atas TIDAK PERNAH benar-benar menyerah
    // (selalu lanjut ke kandidat berikutnya, muter lagi dari awal kalau
    // semua habis), di sini cukup tunggu selama-lamanya sampai BENAR-BENAR
    // konek -- tidak ada lagi jalur "gagal total" / WIFI_FAIL_BIT.
    xEventGroupWaitBits(s_wifi_event_group,
                        WIFI_CONNECTED_BIT,
                        pdFALSE,   // jangan clear bit saat keluar -- biar status tetap kebaca
                        pdTRUE,
                        portMAX_DELAY);

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished, connected ke \"%s\".",
             s_wifi_candidates[s_candidate_idx % WIFI_CANDIDATE_COUNT].ssid);

    return ESP_OK;
}