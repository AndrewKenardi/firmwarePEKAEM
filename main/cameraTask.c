#include "cameraTask.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_camera.h"

// FIX 1: Naikkan XCLK ke 20 MHz (Standar OV2640)
#define CONFIG_XCLK_FREQ 20000000      
#define JPEG_QUALITY 5
#define FB_COUNT 2
static const char *TAG_I2C_DIAG = "CAM_I2C_DIAG";

#define CAM_PWR_GPIO CAM_PIN_PWDN 

static const char *TAG = "CAM_TASK";

extern QueueHandle_t frame_queue;
extern EventGroupHandle_t cameraEventGroup;

// Dipakai bersama vTaskCameraRead (di sini) & vTaskMLStream (wifiStreamTask.c)
// supaya keduanya tidak memanggil esp_camera_fb_get()/fb_return() secara
// bersamaan dari task berbeda (driver esp32-camera tidak resmi thread-safe
// untuk itu).
SemaphoreHandle_t camera_capture_mutex = NULL;

/**
 * @brief Inisialisasi Driver Kamera
 */
esp_err_t init_camera_driver(void)
{
    camera_config_t camera_config = {
        .pin_pwdn  = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,

        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = CONFIG_XCLK_FREQ, // 20 MHz
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,

        .jpeg_quality = JPEG_QUALITY,
        .fb_count = FB_COUNT,
        .grab_mode = CAMERA_GRAB_LATEST
    };

    // Beri jeda sebentar sebelum memanggil init
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init gagal: 0x%x", err);
        return err;
    }

    if (camera_capture_mutex == NULL) {
        camera_capture_mutex = xSemaphoreCreateMutex();
    }

    return ESP_OK;
}

/**
 * @brief Task pembacaan kamera yang aman dari crash
 */
void vTaskCameraRead(void *pvParameters)
{
    ESP_LOGI(TAG, "Memulai Task Pembacaan Kamera...");

    for (;;) {
        // FIX 2: Cek apakah Queue valid sebelum eksekusi capture
        if (frame_queue == NULL) {
            ESP_LOGE(TAG, "frame_queue masih NULL! Menunggu inisialisasi queue...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (camera_capture_mutex != NULL) {
            xSemaphoreTake(camera_capture_mutex, portMAX_DELAY);
        }

        // Ambil frame buffer dari kamera
        camera_fb_t *fb = esp_camera_fb_get();

        if (camera_capture_mutex != NULL) {
            xSemaphoreGive(camera_capture_mutex);
        }

        // FIX 3: Validasi hasil capture
        if (!fb) {
            ESP_LOGE(TAG, "Gagal mengambil frame buffer (fb == NULL)");
            vTaskDelay(pdMS_TO_TICKS(100)); // Beri jeda sebelum mencoba lagi
            continue;
        }

        // FIX 4: Kirim frame pointer ke Queue dengan penanganan yang aman
        if (xQueueSend(frame_queue, &fb, 0) != pdTRUE) {
            ESP_LOGW(TAG, "Queue penuh, melepaskan frame ini...");
            if (camera_capture_mutex != NULL) {
                xSemaphoreTake(camera_capture_mutex, portMAX_DELAY);
            }
            esp_camera_fb_return(fb); // Kembalikan buffer agar tidak leak
            if (camera_capture_mutex != NULL) {
                xSemaphoreGive(camera_capture_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Target ~20 fps
    }
}