#ifndef UART_TX_H
#define UART_TX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pastikan deklarasi ini sesuai dengan nama fungsi di uartTx.c
bool master_uart_send_cmd_with_ack(const char *cmd, uint32_t timeout_ms, uint8_t max_retries);

#ifdef __cplusplus
}
#endif

#endif // UART_TX_H