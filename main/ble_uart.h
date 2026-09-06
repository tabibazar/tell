#ifndef BLE_UART_H
#define BLE_UART_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* Invoked once per complete message. `text` is NUL-terminated and valid only
   for the duration of the call. A zero-length message means "clear". */
typedef void (*ble_uart_cb_t)(const char *text, size_t len);

/* Invoked when the client syncs the clock. `secs` is seconds since local
   midnight, which avoids having to convey a timezone. */
typedef void (*ble_uart_time_cb_t)(uint32_t secs);

/* Starts the NimBLE peripheral advertising the Nordic UART Service. */
esp_err_t ble_uart_start(ble_uart_cb_t on_message, ble_uart_time_cb_t on_time);

#endif /* BLE_UART_H */
