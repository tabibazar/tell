/*
 * The two boards whose ST7789 hangs off SPI. Same transport, same panel
 * driver, different wiring and different glass -- so the constants are per
 * board and everything below them is shared. The CrowPanel (RGB) and lilly
 * (i80) are genuinely different buses and have their own files.
 */
#include "display.h"

#include "canvas.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>


#ifdef CONFIG_SCREEN_BOARD_WAVESHARE_147B
/* Waveshare ESP32-S3-LCD-1.47B, from their own Display_ST7789.h. */
#define PIN_TFT_CS    42
#define PIN_TFT_DC    41
#define PIN_TFT_RST   39
#define PIN_TFT_BL    46
#define PIN_SCK       40
#define PIN_MOSI      45
#define LCD_W 320
#define LCD_H 168
#define LCD_PCLK_HZ (80 * 1000 * 1000)
/* Waveshare give Offset_X 34 for the panel stood upright. Turned landscape
   the two swap, so the 172 rows sit centred in the controller's 240:
   (240 - 172) / 2 = 34, which is their number arrived at independently. */
#define LCD_GAP_X 0
#define LCD_GAP_Y 34
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y true
/* No switched rail: the panel is powered whenever the board is. */
#undef PIN_TFT_POWER

#elif defined(CONFIG_SCREEN_BOARD_NICEMCU_28)
/* NiceMCU-32S-DEV 2.8IPS, from the vendor's include/nicemcu/board_config.h. */
#define PIN_TFT_CS    15
#define PIN_TFT_DC    12
#define PIN_TFT_RST    2
#define PIN_TFT_BL    25
#define PIN_SCK       14
#define PIN_MOSI      13
/*
 * The panel is 240x320 and we drive it on its side, so landscape it is
 * 320x240 -- 26 columns by 10 rows. We cannot have all of it.
 *
 * A full framebuffer there is 153,600 bytes and this board's largest
 * contiguous DMA block is 110,592: no PSRAM, and the heap comes up in
 * fragments. So the panel is letterboxed to 172 rows, which needs 110,080
 * bytes -- which the allocator still refused, because the largest free block
 * is not all allocatable once its own bookkeeping is taken out. 168 rows is
 * 107,520 bytes and does fit, and 168 is still exactly seven rows of the
 * 24-pixel font: the same 26x7 wave had, so every page written for her works
 * here untouched. The 72 unused rows are split top and bottom by the gap.
 */
#define LCD_W 320
#define LCD_H 168
/*
 * The vendor drives it at 24 MHz. A plain ESP32's SPI can go faster, but
 * this is their number on their wiring and a full frame at 24 MHz is about
 * 50 ms, which is fine for a page that changes once a second.
 */
#define LCD_PCLK_HZ (24 * 1000 * 1000)
/* Centred in the 240 the controller drives: (240 - 168) / 2 = 36. */
#define LCD_GAP_X 0
#define LCD_GAP_Y 36
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y false
/* No switched rail. */
#undef PIN_TFT_POWER

/*
 * GPIO12 is the data/command line here, and on a plain ESP32 GPIO12 is MTDI,
 * which is read at every reset to choose the flash voltage: low for 3.3 V,
 * high for 1.8 V. esptool confirms this board leaves it to the strapping pin
 * rather than an eFuse. So a reset while DC is high would have the chip go
 * looking for 1.8 V flash and fail to boot -- a board that appears dead with
 * nothing wrong with it.
 *
 * esp_lcd leaves DC low between transfers, and the board has a pull-down for
 * the moment of reset, which is why it starts at all. Worth knowing before
 * anyone reassigns this pin or adds a pull-up to it.
 */

#else
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
#define LCD_PCLK_HZ (40 * 1000 * 1000)
/* Landscape: with swap_xy the 240px axis maps to the controller's 320-long
   axis (offset 40) and the 135px axis to the 240-long one (offset 53). */
#define LCD_GAP_X 40
#define LCD_GAP_Y 53
#define LCD_MIRROR_X true
#define LCD_MIRROR_Y false
#endif
#define LCD_HOST SPI2_HOST
static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

static void backlight_init(void);

esp_err_t display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_BL)
#ifdef PIN_TFT_POWER
                      | (1ULL << PIN_TFT_POWER)
#endif
        ,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
#ifdef PIN_TFT_POWER
    /* The panel is dead until this rail is up; the most common failure here. */
    gpio_set_level(PIN_TFT_POWER, 1);
#endif
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
        .pclk_hz = LCD_PCLK_HZ,
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
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /*
     * The framebuffer goes to PSRAM where there is any, and internal DMA
     * memory where there is not.
     *
     * It fits internal memory perfectly well on its own -- it did for months
     * -- but it is 107 KB of the ~220 KB there is, and the WiFi radio needs
     * internal DMA memory of its own that cannot come from anywhere else.
     * Two things that both must be internal do not fit; one of them can live
     * elsewhere, and a buffer written once a frame and read by DMA is far
     * less bothered by the move than a radio would be.
     */
#if CONFIG_SPIRAM
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
#else
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
#endif
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no memory for a %u byte framebuffer", 
                 (unsigned)(LCD_W * LCD_H * sizeof(uint16_t)));
        ESP_LOGE(TAG, "  DMA           largest %u  free %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        ESP_LOGE(TAG, "  DMA + 8-bit   largest %u  free %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        ESP_LOGE(TAG, "  internal 8bit largest %u  free %u",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, 1);

    backlight_init();
    display_set_brightness(CONFIG_SCREEN_BRIGHTNESS);

    display_show_text(NULL);
    ESP_LOGI(TAG, "ST7789 up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, s_canvas.cols, s_canvas.rows);
    return ESP_OK;
}

/*
 * The backlight on a timer rather than a pin held high. 1 kHz at ten bits is
 * what the board's own demo uses; well above anything the eye can see as
 * flicker, and coarse enough to cost nothing.
 */
static void backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) return;
    ledc_channel_config_t ch = {
        .gpio_num = PIN_TFT_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = (1 << 10) - 1,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

void display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = ((1u << 10) - 1u) * (uint32_t)percent / 100u;
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty) != ESP_OK) return;
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "backlight %d%%", percent);
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
