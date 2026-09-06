#include "display.h"
#include "ble_uart.h"
#include "timecalc.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>

static const char *TAG = "main";

/* A message owns the screen for this long, then the clock takes it back. */
#define MESSAGE_HOLD_US (30 * 1000000LL)
#define TICK_MS 500

static uint32_t s_base_secs;      /* seconds since local midnight at sync */
static int64_t  s_base_us;        /* esp_timer reading at that moment */
static bool     s_synced;

static int64_t  s_message_us;     /* when the current message arrived */
static bool     s_showing_message;

/* Minute currently drawn, so the panel is only redrawn when it changes.
   -1 forces a redraw; the unsynced placeholder uses its own sentinel. */
#define REDRAW_NEEDED (-1)
#define SHOWING_UNSYNCED (-2)
static int s_drawn_minute = REDRAW_NEEDED;

static void on_time(uint32_t secs)
{
    s_base_secs = secs;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    s_drawn_minute = REDRAW_NEEDED;
}

static void on_message(const char *text, size_t len)
{
    if (len == 0) {
        /* An empty message means "clear", which here means "back to the clock". */
        ESP_LOGI(TAG, "cleared; returning to clock");
        s_showing_message = false;
        s_drawn_minute = REDRAW_NEEDED;
        return;
    }
    ESP_LOGI(TAG, "showing %u bytes", (unsigned)len);
    display_show_text(text);
    s_showing_message = true;
    s_message_us = esp_timer_get_time();
}

static void draw_clock_if_changed(int64_t now)
{
    if (!s_synced) {
        if (s_drawn_minute != SHOWING_UNSYNCED) {
            s_drawn_minute = SHOWING_UNSYNCED;
            display_show_big("--:--");
        }
        return;
    }

    uint32_t secs = timecalc_advance(s_base_secs, (uint64_t)(now - s_base_us));
    int h, m;
    timecalc_split(secs, &h, &m);
    int minute = h * 60 + m;
    if (minute == s_drawn_minute) return;

    char buf[6];
    timecalc_format(secs, buf);
    display_show_big(buf);
    s_drawn_minute = minute;
}

void app_main(void)
{
    esp_err_t err = display_init();
    if (err != ESP_OK) {
        /* A panel that failed to start cannot report its own failure. */
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    if (ble_uart_start(on_message, on_time) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        display_show_text("BLE FAILED");
        return;
    }

    int64_t last_beat = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (s_showing_message && now - s_message_us > MESSAGE_HOLD_US) {
            s_showing_message = false;
            s_drawn_minute = REDRAW_NEEDED;
        }
        if (!s_showing_message) draw_clock_if_changed(now);

        /* USB-Serial-JTAG drops output when no host is attached, so the boot
           log is often missed. A heartbeat makes liveness observable. */
        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            ESP_LOGI(TAG, "alive, clock %s", s_synced ? "synced" : "unset");
        }
    }
}
