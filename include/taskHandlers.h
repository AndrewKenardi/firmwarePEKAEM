// FREERTOS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define CAMERA_STACK_SIZE (10 * 1024)
#define LCD_STACK_SIZE (4 * 1024)
#define HTTP_STACK_SIZE (8 * 1024)

StackType_t xCameraReadStack[CAMERA_STACK_SIZE];
StaticTask_t xCameraReadTaskBuffer;
StackType_t xHttpStreamStack[HTTP_STACK_SIZE];
StaticTask_t xHttpStreamTaskBuffer;
StackType_t xLcdStatusStack[LCD_STACK_SIZE];
StaticTask_t xLcdStatusTaskBuffer;