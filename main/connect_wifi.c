#include "connect_wifi.h"

int wifi_connect_status = 0;
static const char *WIFI_TAG = "Connect_WiFi";
int s_retry_num = 0;


#define WIFI_SSID "TRIKBB" // TP-Link tau TRIKBB
#define WIFI_PASSWORD "pemadamkebakaran" //12341234 atau pemadamkebakaran
#define MAXIMUM_RETRY 5
/* FreeRTOS event group to signal when we are connected*/
EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        wifi_connect_status = 0;
        ESP_LOGI(WIFI_TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_connect_status = 1;
    }
}

esp_err_t connect_wifi(void)
{
    esp_err_t err;
    s_wifi_event_group = xEventGroupCreate();

    // --- Idempotent init guards ---
    // esp_netif_init() / esp_event_loop_create_default() must only run once
    // for the lifetime of the program. connect_wifi() may legitimately be
    // called more than once (e.g. a retry loop in the caller), so we treat
    // "already initialized" as success rather than letting ESP_ERROR_CHECK
    // abort the whole device.
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // esp_netif_create_default_wifi_sta() and esp_wifi_init() are also not
    // safe to call twice. Guard with a static flag so a second call to
    // connect_wifi() (e.g. after a transient failure) doesn't re-init wifi
    // from scratch while it's already running.
    static bool s_wifi_stack_initialized = false;
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

        wifi_config_t wifi_config = {
            .sta = {
                .ssid = WIFI_SSID,
                .password = WIFI_PASSWORD,
                /* Setting a password implies station will connect to all security modes including WEP/WPA.
                 * However these modes are deprecated and not advisable to be used. Incase your Access point
                 * doesn't support WPA2, these mode can be enabled by commenting below line */
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());

        s_wifi_stack_initialized = true;
    }
    else
    {
        // Wifi stack already running from a previous call; just kick off
        // a fresh connection attempt and let the event handler drive it.
        s_retry_num = 0;
        esp_wifi_connect();
    }

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");

    // Disable WiFi modem sleep / power save. Power save mode can introduce
    // multi-second latency spikes when the radio "wakes up" to send/receive
    // a packet, which matches symptoms like sudden 10+ second HTTP stalls.
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    vEventGroupDelete(s_wifi_event_group);

    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(WIFI_TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
        // FIX: success must report ESP_OK, not ESP_FAIL. The caller's
        // `while (connect_wifi() == ESP_FAIL)` loop was re-entering this
        // function (and re-running the init calls above) even after a
        // successful connection, which is what triggered the
        // ESP_ERR_INVALID_STATE abort.
        return ESP_OK;
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(WIFI_TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
        return ESP_FAIL;
    }
    else
    {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
        return ESP_FAIL;
    }
}