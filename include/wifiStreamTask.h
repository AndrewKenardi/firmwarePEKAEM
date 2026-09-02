#ifndef WIFI_STREAM_TASK_H
#define WIFI_STREAM_TASK_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ml_stream_init(void);
void ml_stream_set_distance(uint16_t distance_mm, bool valid);
void vTaskMLStream(void *pvParameters);
void vTaskWifiConnect(void *pvParameter);

#ifdef __cplusplus
}
#endif

#endif // WIFI_STREAM_TASK_H