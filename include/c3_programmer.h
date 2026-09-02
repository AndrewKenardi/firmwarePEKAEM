#ifndef C3_PROGRAMMER_H
#define C3_PROGRAMMER_H

#include "esp_err.h"
#include <stdbool.h>

static void master_uart_init(void);
void master_ota_task(void *pvParameters);

#endif // C3_PROGRAMMER_H