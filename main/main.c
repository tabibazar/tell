#include "display.h"
#include "ble_uart.h"
#include "gt911.h"
#include "pages.h"
#include "timecalc.h"
#include "usagedata.h"
#include "views.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch poll interval */
#define MESSAGE_MAX 512

static uint32_t s_base_secs;
static int64_t  s_base_us;
static bool     s_synced;

static usagedata_t s_data;
static char s_message[MESSAGE_MAX + 1];
static pages_t s_pages;

/* Forces a redraw when the page or the displayed second changes. */
static page_t s_drawn_page = PAGE_COUNT;
static int s_drawn_second = -1;

static void on_time(uint32_t secs)
{
    s_base_secs = secs;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    s_drawn_second = -1;
}

static void on_message(const char *text, size_t len)
{
    int64_t now = esp_timer_get_time();

    ud_kind_t kind = usagedata_parse(&s_data, text);
    if (kind == UD_STATS || kind == UD_DAILY) {
        /* Data arrives on a timer, so it must never steal the view: refresh
           the numbers, and redraw only if that page is already showing. */
        page_t target = (kind == UD_STATS) ? PAGE_STATS : PAGE_DAILY;
        if (s_pages.current == target) s_drawn_page = PAGE_COUNT;
        return;
    }
    if (len == 0) {
        s_message[0] = '\0';
        pages_show(&s_pages, PAGE_CLOCK, now);
    } else {
        /* A message is someone talking to you, so it does take the view. */
        size_t n = len < MESSAGE_MAX ? len : MESSAGE_MAX;
        memcpy(s_message, text, n);
        s_message[n] = '\0';
        pages_show(&s_pages, PAGE_MESSAGE, now);
    }
    s_drawn_page = PAGE_COUNT;    /* force a redraw */
}

static void draw_clock(canvas_t *c, int64_t now)
{
    if (!s_synced) {
        if (s_drawn_second != -2) {
            s_drawn_second = -2;
            canvas_big(c, "--:--:--");
            display_blit();
        }
        return;
    }
    uint32_t secs = timecalc_advance(s_base_secs, (uint64_t)(now - s_base_us));
    if ((int)secs == s_drawn_second) return;
    char buf[9];
    timecalc_format_hms(secs, buf);
    canvas_big(c, buf);
    display_blit();
    s_drawn_second = (int)secs;
}

void app_main(void)
{
    if (display_init() != ESP_OK) {
        /* A panel that failed to start cannot report its own failure. */
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    unsigned available = PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE);
    bool touch = false;
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    available |= PAGE_BIT(PAGE_STATS) | PAGE_BIT(PAGE_DAILY);
    touch = gt911_init() == ESP_OK;
#endif
    pages_init(&s_pages, available);

    canvas_t *c = display_canvas();

    if (ble_uart_start(on_message, on_time) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        canvas_text(c, "BLE FAILED");
        display_blit();
        return;
    }

    int64_t last_beat = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();

        if (touch && gt911_tapped()) {
            page_t p = pages_advance(&s_pages, now);
            ESP_LOGI(TAG, "tap -> page %d", (int)p);
        }

        if (pages_idle_expired(&s_pages, now)) {
            pages_show(&s_pages, PAGE_CLOCK, now);
        }

        if (s_pages.current != s_drawn_page) {
            s_drawn_page = s_pages.current;
            s_drawn_second = -1;
            switch (s_pages.current) {
            case PAGE_STATS:   views_stats(c, &s_data); display_blit(); break;
            case PAGE_DAILY:   views_daily(c, &s_data); display_blit(); break;
            case PAGE_MESSAGE:
                /* A blank page says nothing about what it is or why it is
                   empty, so name it rather than showing nothing. */
                /* This page carries Claude's running commentary while it
                   works. Name it, so an empty one reads as "idle" rather
                   than as a broken screen. */
                canvas_text(c, s_message[0] ? s_message
                                            : "CLAUDE\n\nidle -- no update yet");
                display_blit();
                break;
            default: break;
            }
        }
        if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);

        /* USB-Serial-JTAG drops output when no host is attached, so the boot
           log is often missed. A heartbeat makes liveness observable. */
        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            ESP_LOGI(TAG, "alive, page %d, clock %s",
                     (int)s_pages.current, s_synced ? "synced" : "unset");
        }
    }
}
