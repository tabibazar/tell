/* LilyGO T-Display-S3: ST7789 320x170 over an 8-bit i80 parallel bus. */
#include "display.h"

#include "canvas.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>


/* LilyGO T-Display-S3, from their examples/factory/pin_config.h. */
#define PIN_LCD_D0    39
#define PIN_LCD_D1    40
#define PIN_LCD_D2    41
#define PIN_LCD_D3    42
#define PIN_LCD_D4    45
#define PIN_LCD_D5    46
#define PIN_LCD_D6    47
#define PIN_LCD_D7    48
#define PIN_LCD_WR     8
#define PIN_LCD_RD     9
#define PIN_LCD_DC     7
#define PIN_LCD_CS     6
#define PIN_LCD_RST    5
#define PIN_LCD_BL    38
#define PIN_POWER_ON  15

#define LCD_W 320
#define LCD_H 170

/* LilyGO run the bus at 16 MHz and warn that too low or too high a pixel
   clock produces a mosaic pattern on this panel. */
#define LCD_PCLK_HZ (16 * 1000 * 1000)

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

esp_err_t display_init(void)
{
    /* GPIO15 gates the peripheral rail. Without it the panel never lights and
       the board reads as a dead port rather than a missing line. */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_POWER_ON) | (1ULL << PIN_LCD_RD)
                      | (1ULL << PIN_LCD_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_POWER_ON, 1);
    /* RD is unused but must idle high, or the panel sees a read strobe. */
    gpio_set_level(PIN_LCD_RD, 1);
    gpio_set_level(PIN_LCD_BL, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .data_gpio_nums = {
            PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
            PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7,
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_W * LCD_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        /* swap_color_bytes stays off, which is not an oversight. With it off
           the peripheral sends the framebuffer in memory order, low byte
           first -- exactly what the Feather's SPI path does, and the palette
           is already field-tested there on the level page's greens and
           oranges. Turning it on here would make lilly the odd one out.
           If colour comes out wrong on this panel and right on the Feather,
           this flag is the first thing to try, but then the two boards
           disagree and one of them is lying. */
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io_handle));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* Landscape: swap_xy maps the 320px axis onto the controller's 320-long
       axis, leaving the 170px axis centred in its 240 (offset 35). */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 35));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 106 KB, which fits internal DMA memory, so this board needs no PSRAM. */
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no DMA memory for framebuffer");
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, 1);

    display_show_text(NULL);
    ESP_LOGI(TAG, "i80 ST7789 up: %dx%d, %d cols x %d rows",
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
