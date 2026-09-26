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
#define JPEG_QUALITY 16
#define FB_COUNT 2
static const char *TAG_I2C_DIAG = "CAM_I2C_DIAG";

#define CAM_PWR_GPIO CAM_PIN_PWDN 

static const char *TAG = "CAM_TASK";

volatile uint32_t g_cam_stage = 0;  // 1=di dalam fb_get, 2=setelah fb_get, 3=xQueueSend, 4=vTaskDelay
volatile uint32_t g_cam_iter = 0, g_cam_fb_ok = 0, g_cam_fb_null = 0, g_cam_q_ok = 0, g_cam_q_drop = 0;

extern QueueHandle_t frame_queue;
extern EventGroupHandle_t cameraEventGroup;

// Dipakai bersama vTaskCameraRead (di sini) & vTaskMLStream (wifiStreamTask.c)
// supaya keduanya tidak memanggil esp_camera_fb_get()/fb_return() secara
// bersamaan dari task berbeda (driver esp32-camera tidak resmi thread-safe
// untuk itu).
SemaphoreHandle_t camera_capture_mutex = NULL;

static camera_fb_t *camera_safe_fb_get(void)
{
    if (camera_capture_mutex == NULL ||
        xSemaphoreTake(camera_capture_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    xSemaphoreGive(camera_capture_mutex);
    return fb;
}

static void camera_safe_fb_return(camera_fb_t *fb)
{
    if (fb == NULL || camera_capture_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(camera_capture_mutex, portMAX_DELAY) == pdTRUE) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(camera_capture_mutex);
    }
}

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
        .grab_mode = CAMERA_GRAB_LATEST,
        .fb_location = CAMERA_FB_IN_PSRAM
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

    int64_t t0 = esp_timer_get_time();

    for (;;) {
        // STAT di awal loop supaya selalu tercetak
        if (esp_timer_get_time() - t0 >= 2000000) {
            ESP_LOGI(TAG, "STAT 2s: iter=%u fb_ok=%u fb_null=%u q_ok=%u q_drop=%u",
                     (unsigned)g_cam_iter, (unsigned)g_cam_fb_ok,
                     (unsigned)g_cam_fb_null, (unsigned)g_cam_q_ok,
                     (unsigned)g_cam_q_drop);
            g_cam_iter = g_cam_fb_ok = g_cam_fb_null = g_cam_q_ok = g_cam_q_drop = 0;
            t0 = esp_timer_get_time();
        }

        if (frame_queue == NULL) {
            ESP_LOGE(TAG, "frame_queue masih NULL! Menunggu inisialisasi queue...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        g_cam_iter++;
        g_cam_stage = 1;
        camera_fb_t *fb = camera_safe_fb_get();
        g_cam_stage = 2;

        if (!fb) {
            g_cam_fb_null++;
            ESP_LOGE(TAG, "Gagal mengambil frame buffer (fb == NULL)");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        g_cam_fb_ok++;

        g_cam_stage = 3;
        if (xQueueSend(frame_queue, &fb, 0) != pdTRUE) {
            g_cam_q_drop++;
            camera_safe_fb_return(fb);
        } else {
            g_cam_q_ok++;
        }

        g_cam_stage = 4;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}