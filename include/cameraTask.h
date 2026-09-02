#ifndef CAMERA_TASK_H_
#define CAMERA_TASK_H_


#include "esp_camera.h"
#include "pins.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define IS_CAMERA_CONNECTED_BIT (1 << 0)
#define IS_CAMERA_FB_OV_BIT (1 << 0)
#define IS_CAMERA_READING (1 << 0)

// Driver esp32-camera tidak resmi thread-safe untuk esp_camera_fb_get()/
// esp_camera_fb_return() dipanggil dari lebih dari satu task sekaligus.
// Karena sekarang ada 2 task yang menangkap frame (vTaskCameraRead untuk
// robot-command, vTaskMLStream untuk ML server), mutex ini WAJIB dipegang
// di sekeliling pasangan fb_get()...fb_return() di kedua task tsb.
// Dibuat di init_camera_driver().
extern SemaphoreHandle_t camera_capture_mutex;

esp_err_t init_camera_driver(void);
void vTaskCameraRead(void *pvParameters);

#endif