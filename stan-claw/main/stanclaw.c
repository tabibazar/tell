/*
 * stan-claw: tap, speak, and it answers aloud and on screen (Deepgram,
 * Claude with a remote MCP server, ElevenLabs); and an MCP server on the
 * local network for its speaker, mics and screen. On a Waveshare
 * ESP32-S3-Touch-LCD-4B. docs/superpowers/specs/2026-10-10-stan-claw-design.md
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "lcd.h"
#include "touch.h"

static const char *TAG = "stanclaw";

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(lcd_init());
    canvas_fill_rect(lcd_canvas(), 0, 0, 480, 480, 0x0841);
    lcd_show();
    ESP_LOGI(TAG, "touch %s", touch_init() == ESP_OK ? "ok" : "missing");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
