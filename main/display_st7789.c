/* Adafruit Feather ESP32-S3 TFT: ST7789 240x135 over SPI. */
#include "display.h"

#include "canvas.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>


/* Adafruit Feather ESP32-S3 TFT, from the board's Arduino variant. */
#define PIN_TFT_POWER 21
#define PIN_TFT_CS     7
#define PIN_TFT_DC    39
#define PIN_TFT_RST   40
#define PIN_TFT_BL    45
#define PIN_SCK       36
#define PIN_MOSI      35

#define LCD_W 240
#define LCD_H 135
#define LCD_HOST SPI2_HOST
static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

esp_err_t display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_POWER) | (1ULL << PIN_TFT_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    /* The panel is dead until this rail is up; the most common failure here. */
    gpio_set_level(PIN_TFT_POWER, 1);
    gpio_set_level(PIN_TFT_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2 + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_TFT_DC,
        .cs_gpio_num = PIN_TFT_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* Landscape: with swap_xy the 240px axis maps to the controller's 320-long
       axis (offset 40) and the 135px axis to the 240-long one (offset 53). */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 40, 53));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no DMA memory for framebuffer");
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, 1);

    display_show_text(NULL);
    ESP_LOGI(TAG, "ST7789 up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, s_canvas.cols, s_canvas.rows);
    return ESP_OK;
}

canvas_t *display_canvas(void) { return &s_canvas; }

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
