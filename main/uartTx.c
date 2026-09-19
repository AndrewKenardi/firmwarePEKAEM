#include "uartTx.h"
#include "c3_programmer.h"
#include "driver/uart.h"
#include "driver/gpio.h" // Memastikan GPIO_NUM_1 dan GPIO_NUM_3 terdefinisi
#include "esp_log.h"
#include <string.h>

static const char *TAG = "MASTER_UART";


// Samakan tipe kembalian (bool) dan parameternya dengan uartTx.h
bool master_uart_send_cmd_with_ack(const char *cmd, uint32_t timeout_ms, uint8_t max_retries)
{
    if (cmd == NULL) return false;

    char tx_payload[32];
    snprintf(tx_payload, sizeof(tx_payload), "CMD:%s\n", cmd);

    for (uint8_t attempt = 1; attempt <= max_retries; attempt++) {
        uart_flush_input(MASTER_UART_NUM);
        uart_write_bytes(MASTER_UART_NUM, tx_payload, strlen(tx_payload));

        uint8_t rx_buf[64];
        int rx_bytes = 0;
        uint32_t start_time = xTaskGetTickCount();

        while ((xTaskGetTickCount() - start_time) < pdMS_TO_TICKS(timeout_ms)) {
            int len = uart_read_bytes(MASTER_UART_NUM, rx_buf + rx_bytes, 1, pdMS_TO_TICKS(20));
            if (len > 0) {
                if (rx_buf[rx_bytes] == '\n' || rx_buf[rx_bytes] == '\r') {
                    rx_buf[rx_bytes] = '\0';
                    break;
                }
                rx_bytes += len;
                if (rx_bytes >= sizeof(rx_buf) - 1) break;
            }
        }

        if (rx_bytes > 0) {
            rx_buf[rx_bytes] = '\0';
            if (strstr((char *)rx_buf, "ACK") != NULL) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return false;
}