#include "display.h"
#include "ble_uart.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void on_message(const char *text, size_t len)
{
    ESP_LOGI(TAG, "showing %u bytes", (unsigned)len);
    display_show_text(len > 0 ? text : NULL);
}

void app_main(void)
{
    esp_err_t err = display_init();
    if (err != ESP_OK) {
        /* A panel that failed to start cannot report its own failure. */
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }
    display_show_text("READY");

    if (ble_uart_start(on_message) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        display_show_text("BLE FAILED");
    }

    /* USB-Serial-JTAG drops output when no host is attached, so the boot log
       is often missed. A heartbeat makes liveness observable at any time. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive");
    }
}
