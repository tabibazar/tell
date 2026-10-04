/*
 * tiny1: a clone of Adafruit's Feather ESP32-S3 TFT, showing spy's photos.
 *
 * spy sends each minute's frame over ESP-NOW (main/camlink.h); tiny1 puts
 * the chunks back together, decodes the JPEG at half size and fits it to its
 * 240x135 screen (180x135, the camera being 4:3), with the time it was taken
 * in the corner. When frames stop -- spy gone to work, or off -- the last one
 * stays with "last seen" under it. BOOT asks spy for a fresh photo.
 *
 * Pins as found on the board (2026-10-03) and as on Adafruit's design:
 *   GPIO21  TFT + I2C power (high = on)
 *   GPIO7 CS, 39 DC, 40 RST, 45 backlight, 36 SCK, 35 MOSI: the ST7789
 *   GPIO42 SDA, 41 SCL: BMP280 0x77, QMI8658 0x6B (not used yet)
 *   GPIO33 NeoPixel data, 34 its power: kept dark
 *   GPIO0   BOOT button (low when pressed)
 */
#include <stdio.h>
#include <string.h>

#include "camlink.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "font_12x24.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "led_strip.h"
#include "nvs_flash.h"

static const char *TAG = "tiny1";
static const uint8_t SPY[6] = CAMLINK_SPY_MAC;

#define W 240
#define H 135
#define IMG_W 180                    /* 4:3 at the screen's height */
#define IMG_X ((W - IMG_W) / 2)

/* ---- the screen ---------------------------------------------------------- */

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;               /* panel byte order (big-endian RGB565) */

static inline uint16_t px(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((c >> 8) | (c << 8));
}

static void screen_init(void)
{
    gpio_config_t out = { .pin_bit_mask = (1ULL << 21) | (1ULL << 45), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(21, 1);           /* TFT + I2C power */
    vTaskDelay(pdMS_TO_TICKS(50));
    spi_bus_config_t bus = { .sclk_io_num = 36, .mosi_io_num = 35, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * H * 2 + 64 };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = { .dc_gpio_num = 39, .cs_gpio_num = 7, .pclk_hz = 40 * 1000 * 1000,
                                          .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io));
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = 40, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16 };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    /* The 1.14" glass is 135x240 portrait on a 240x320 controller: landscape
       is a swap of x and y, and a gap of 40 and 53 (Adafruit's offsets). */
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_set_gap(s_panel, 40, 53);
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_fb = heap_caps_malloc(W * H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    memset(s_fb, 0, W * H * 2);
    gpio_set_level(45, 1);           /* backlight */
}

static void screen_show(void)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, W, H, s_fb);
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h && yy < H; yy++)
        for (int xx = x; xx < x + w && xx < W; xx++)
            if (xx >= 0 && yy >= 0) s_fb[yy * W + xx] = c;
}

/* The project's 12x24 font: ink in the low 12 bits of each row, rows 3..19. */
static int text(int x, int y, const char *s, uint16_t fg)
{
    for (; *s; s++, x += 12) {
        if (*s < FONT_FIRST || *s > FONT_LAST) continue;
        const uint8_t *g = font_glyphs[*s - FONT_FIRST];
        for (int gy = 3; gy < 20; gy++) {
            unsigned row = ((unsigned)g[2 * gy] << 8) | g[2 * gy + 1];
            for (int gx = 0; gx < 12; gx++)
                if (row & (0x800u >> gx)) {
                    int xx = x + gx, yy = y + gy - 3;
                    if (xx >= 0 && xx < W && yy >= 0 && yy < H) s_fb[yy * W + xx] = fg;
                }
        }
    }
    return x;
}

static void label(int x, int y, const char *s, uint16_t fg)
{
    fill(x - 2, y - 2, 12 * (int)strlen(s) + 4, 21, px(0, 0, 0));
    text(x, y, s, fg);
}

/* ---- the link ------------------------------------------------------------ */

static uint8_t *s_rx[2];             /* frames being assembled / last whole */
static int s_rx_at;                  /* the one being assembled */
static uint32_t s_rx_got, s_rx_total;
static uint16_t s_rx_frame;
static camlink_hdr_t s_whole_hdr;
static volatile int s_whole = -1;    /* index of a finished frame not yet shown */
static SemaphoreHandle_t s_ready, s_sent;
static volatile bool s_send_ok;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (memcmp(info->src_addr, SPY, 6) != 0 || len < (int)sizeof(camlink_hdr_t)) return;
    camlink_hdr_t h;
    memcpy(&h, data, sizeof h);
    if (h.magic != CAMLINK_MAGIC || h.type != CAMLINK_PART) return;
    if (h.total > CAMLINK_MAX || h.offset + h.len > h.total || (int)(sizeof h + h.len) > len) return;
    if (h.frame != s_rx_frame || h.offset == 0) {    /* a new photo starts */
        s_rx_frame = h.frame;
        s_rx_got = 0;
        s_rx_total = h.total;
    }
    memcpy(s_rx[s_rx_at] + h.offset, data + sizeof h, h.len);
    s_rx_got += h.len;
    if (s_rx_got == s_rx_total) {
        s_whole_hdr = h;
        s_whole = s_rx_at;
        s_rx_at ^= 1;                                /* the next goes in the other */
        s_rx_got = 0;
        s_rx_frame = 0;
        xSemaphoreGive(s_ready);
    }
}

static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t st)
{
    (void)info;
    s_send_ok = st == ESP_NOW_SEND_SUCCESS;
    xSemaphoreGive(s_sent);
}

static void radio_start(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_channel(CAMLINK_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);
    esp_now_peer_info_t p = { .channel = CAMLINK_CHANNEL, .ifidx = WIFI_IF_STA, .encrypt = false };
    memcpy(p.peer_addr, SPY, 6);
    esp_now_add_peer(&p);
}

/* BOOT: ask spy for a photo, again and again for up to three seconds, since
   spy's radio sleeps between short wake windows. */
static bool ask_spy(void)
{
    camlink_hdr_t h = { .magic = CAMLINK_MAGIC, .type = CAMLINK_ASK };
    int64_t until = esp_timer_get_time() + 3000000;
    while (esp_timer_get_time() < until) {
        xSemaphoreTake(s_sent, 0);
        if (esp_now_send(SPY, (const uint8_t *)&h, sizeof h) == ESP_OK &&
            xSemaphoreTake(s_sent, pdMS_TO_TICKS(100)) == pdTRUE && s_send_ok) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

/* ---- drawing a photo ----------------------------------------------------- */

static uint16_t *s_dec;              /* 320x240 RGB565, the half-size decode */

static bool show_photo(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t jc = { .indata = (uint8_t *)jpg, .indata_size = len, .outbuf = (uint8_t *)s_dec,
                                .outbuf_size = 320 * 240 * 2, .out_format = JPEG_IMAGE_FORMAT_RGB565,
                                .out_scale = JPEG_IMAGE_SCALE_1_2, .flags.swap_color_bytes = 1 };
    esp_jpeg_image_output_t jo;
    if (esp_jpeg_decode(&jc, &jo) != ESP_OK || jo.width == 0 || jo.height == 0) return false;
    /* Fit to 180x135: each screen pixel the decoded pixel under it. */
    memset(s_fb, 0, W * H * 2);
    for (int y = 0; y < H; y++) {
        const uint16_t *src = s_dec + (size_t)(y * (int)jo.height / H) * jo.width;
        uint16_t *dst = s_fb + y * W + IMG_X;
        for (int x = 0; x < IMG_W; x++) dst[x] = src[x * (int)jo.width / IMG_W];
    }
    return true;
}

static void status_line(const char *s, uint16_t fg)
{
    fill(0, H - 20, W, 20, px(0, 0, 0));
    text((W - 12 * (int)strlen(s)) / 2, H - 19, s, fg);
}

static void neopixel_off(void)
{
    led_strip_config_t sc = { .strip_gpio_num = 33, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                              .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_handle_t led;
    gpio_config_t pw = { .pin_bit_mask = 1ULL << 34, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pw);
    gpio_set_level(34, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    if (led_strip_new_rmt_device(&sc, &rc, &led) == ESP_OK) {
        led_strip_clear(led);
        vTaskDelay(pdMS_TO_TICKS(5));
        led_strip_del(led);
    }
    gpio_set_level(34, 0);
}

void app_main(void)
{
    neopixel_off();
    screen_init();
    s_rx[0] = heap_caps_malloc(CAMLINK_MAX, MALLOC_CAP_SPIRAM);
    s_rx[1] = heap_caps_malloc(CAMLINK_MAX, MALLOC_CAP_SPIRAM);
    s_dec = heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    s_ready = xSemaphoreCreateBinary();
    s_sent = xSemaphoreCreateBinary();
    text((W - 12 * 6) / 2, 30, "tiny1", px(255, 255, 255));
    status_line("waiting for spy", px(150, 150, 150));
    screen_show();
    radio_start();
    gpio_config_t boot = { .pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);
    ESP_LOGI(TAG, "waiting for spy on channel %d", CAMLINK_CHANNEL);

    int64_t last_us = 0, asked_us = 0;
    char last[8] = "";
    bool stale_shown = false;
    for (;;) {
        if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(50)) == pdTRUE && s_whole >= 0) {
            int k = s_whole;
            camlink_hdr_t h = s_whole_hdr;
            s_whole = -1;
            if (show_photo(s_rx[k], h.total)) {
                if (h.weekday != 0xFF) snprintf(last, sizeof last, "%02d:%02d", h.hour, h.minute);
                else last[0] = 0;
                if (last[0]) label(W - IMG_X - 12 * 5 - 4, 4, last, px(255, 255, 255));
                screen_show();
                last_us = esp_timer_get_time();
                stale_shown = false;
                asked_us = 0;
                ESP_LOGI(TAG, "photo %u, %u bytes, taken %s", h.frame, (unsigned)h.total, last);
            }
        }
        /* Three minutes without a frame: say when the last one was. */
        if (last_us && !stale_shown && esp_timer_get_time() - last_us > 3LL * 60 * 1000000) {
            char s[24];
            snprintf(s, sizeof s, "last seen %s", last[0] ? last : "--");
            status_line(s, px(255, 170, 0));
            screen_show();
            stale_shown = true;
        }
        /* spy heard the ask but no photo came: the one small ask got through
           where a photo's twenty-odd chunks could not -- the edge of range. */
        if (asked_us && esp_timer_get_time() - asked_us > 20000000) {
            status_line("photo lost: too far?", px(255, 170, 0));
            screen_show();
            ESP_LOGW(TAG, "spy heard the ask but no photo arrived in 20 s");
            asked_us = 0;
        }
        if (!gpio_get_level(0)) {                    /* BOOT */
            status_line("asking spy...", px(120, 200, 255));
            screen_show();
            bool ok = ask_spy();
            status_line(ok ? "spy is taking it" : "spy not in range", ok ? px(120, 255, 120) : px(255, 120, 80));
            screen_show();
            if (ok) asked_us = esp_timer_get_time();
            ESP_LOGI(TAG, "asked spy: %s", ok ? "heard" : "no answer");
            while (!gpio_get_level(0)) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
