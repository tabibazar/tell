/*
 * Elecrow CrowPanel 7.0" HMI: 800x480 TN/IPS panel on the ESP32-S3's 16-bit
 * RGB parallel bus.
 *
 * Pin map and timings are taken from Elecrow's own LovyanGFX configuration in
 * CrowPanel-7.0-HMI-ESP32-Display-800x480, and are identical across their
 * V1.0, V2.0 and V3.0 board revisions.
 *
 * UNVERIFIED: written before the hardware was available. See docs/porting.md.
 */
#include "display.h"

#include "canvas.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_W 800
#define LCD_H 480

/* font.h selects a native 16x32 cell for this board, so no magnification:
   50 columns x 15 rows of sharp glyphs rather than replicated pixels. */
#define TEXT_SCALE 1

#define PIN_BACKLIGHT 2
#define PIN_PCLK      0
#define PIN_HSYNC    39
#define PIN_VSYNC    40
#define PIN_DE       41

/* Blue 0-4, green 0-5, red 0-4: RGB565 order as the panel expects it. */
#define RGB_DATA_PINS { 15, 7, 6, 5, 4, 9, 46, 3, 8, 16, 1, 14, 21, 47, 48, 45 }

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

esp_err_t display_init(void)
{
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(PIN_BACKLIGHT, 1);

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        /* An RGB panel streams from memory continuously. The framebuffer is
           750 KB, far past SRAM, so it lives in PSRAM and a bounce buffer in
           SRAM keeps the DMA fed without stalling on flash access. */
        .bounce_buffer_size_px = LCD_W * 10,
        .psram_trans_align = 64,
        .hsync_gpio_num = PIN_HSYNC,
        .vsync_gpio_num = PIN_VSYNC,
        .de_gpio_num = PIN_DE,
        .pclk_gpio_num = PIN_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = RGB_DATA_PINS,
        .timings = {
            .pclk_hz = 15 * 1000 * 1000,
            .h_res = LCD_W,
            .v_res = LCD_H,
            .hsync_pulse_width = 48,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = 31,
            .vsync_back_porch = 13,
            .vsync_front_porch = 1,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    /* Draw into our own buffer and hand whole frames to the panel, matching
       how the SPI board works, rather than writing the panel's live buffer. */
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the %d KB framebuffer",
                 (LCD_W * LCD_H * 2) / 1024);
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, TEXT_SCALE);

    display_show_text(NULL);
    ESP_LOGI(TAG, "RGB panel up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, s_canvas.cols, s_canvas.rows);
    return ESP_OK;
}

canvas_t *display_canvas(void) { return &s_canvas; }

/* The CrowPanel's backlight is not driven from here, and it is at work
   rather than on this desk, so this is left alone rather than changed
   blind. */
void display_set_brightness(int percent) { (void)percent; }

void display_blit(void)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

void display_show_text(const char *utf8)
{
    if (s_fb == NULL) return;
    canvas_text(&s_canvas, utf8);
    display_blit();
}

void display_show_big(const char *text)
{
    if (s_fb == NULL) return;
    canvas_big(&s_canvas, text);
    display_blit();
}
