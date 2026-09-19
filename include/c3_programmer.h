#ifndef C3_PROGRAMMER_H
#define C3_PROGRAMMER_H

#include "esp_err.h"
#include <stdbool.h>
#include "esp_system.h"
#include "driver/gpio.h" // Menambahkan definisi GPIO_NUM_X


#define MASTER_UART_NUM       (UART_NUM_0)
#define MASTER_TX_PIN         (GPIO_NUM_1)  
#define MASTER_RX_PIN         (GPIO_NUM_3)  
#define CHUNK_SIZE            (512)
#define MAX_RETRIES           (5)


static void master_uart_init(void);
void master_ota_task(void *pvParameters);

#endif // C3_PROGRAMMER_H