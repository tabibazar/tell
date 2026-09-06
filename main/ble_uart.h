#ifndef BLE_UART_H
#define BLE_UART_H

#include "esp_err.h"
#include <stddef.h>

/* Invoked once per complete message. `text` is NUL-terminated and valid only
   for the duration of the call. A zero-length message means "clear". */
typedef void (*ble_uart_cb_t)(const char *text, size_t len);

/* Starts the NimBLE peripheral advertising the Nordic UART Service. */
esp_err_t ble_uart_start(ble_uart_cb_t on_message);

#endif /* BLE_UART_H */
