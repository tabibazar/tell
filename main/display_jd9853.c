/*
 * envo: the Waveshare ESP32-S3-Touch-LCD-1.47, a JD9853 over SPI. Same 172x320
 * IPS glass as wave's ST7789 1.47B, driven landscape 320x172, so the panel
 * geometry (the 34-pixel offset, the mirror, the invert) is wave's -- only the
 * controller and the wiring differ. The JD9853 is not one esp_lcd ships, so its
 * vendor driver is carried in components/esp_lcd_jd9853/ (Espressif's own,
 * Apache-2.0, the one Waveshare's BSP uses). It exposes the same panel ops as
 * ST7789 -- set_gap, mirror, swap_xy -- so everything below the constants is a
 * copy of display_st7789.c's flow.
 *
 * Pins and the panel config come from Waveshare's ESP-IDF BSP for this board
 * (components/esp_bsp/bsp_display.h + bsp_display.c), cross-checked against the
 * board's CircuitPython pin map. Not guessed.
 */
#include "display.h"

#include "canvas.h"

#include "esp_lcd_jd9853.h"

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

/* Waveshare ESP32-S3-Touch-LCD-1.47, from their bsp_display.h. */
#define PIN_TFT_CS    21
#define PIN_TFT_DC    45
#define PIN_TFT_RST   40
#define PIN_TFT_BL    46
#define PIN_SCK       38
#define PIN_MOSI      39
#define LCD_W 320
#define LCD_H 172
#define LCD_PCLK_HZ (80 * 1000 * 1000)
/* The 172-wide panel sits centred in the controller's 240 columns, so its
   column offset is (240 - 172) / 2 = 34. Landscape (swap_xy) that offset falls
   on the y axis, exactly as it does for wave's ST7789 on the same glass. */
#define LCD_GAP_X 0
#define LCD_GAP_Y 34
#define LCD_MIRROR_X false
#define LCD_MIRROR_Y true
/* The JD9853 wants the same colour order and inversion the vendor BSP sets;
   if a test pattern ever shows red/green/blue rotated, flip this to 1 (see the
   NiceMCU note in display_st7789.c for why a byte swap and not a BGR order). */
#define LCD_SWAP_COLOR_BYTES 0

#define LCD_HOST SPI2_HOST
static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

static void backlight_init(void);


esp_err_t display_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TFT_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_jd9853(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, LCD_GAP_Y));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* PSRAM here (unlike wave): this board has it, and the framebuffer keeps
       internal DMA memory free for the radio. */
#if CONFIG_SPIRAM
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
#else
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_DMA);
#endif
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no memory for a %u byte framebuffer",
                 (unsigned)(LCD_W * LCD_H * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, 1);

    backlight_init();
    display_set_brightness(CONFIG_SCREEN_BRIGHTNESS);

    display_show_text(NULL);
    ESP_LOGI(TAG, "JD9853 up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, s_canvas.cols, s_canvas.rows);
    return ESP_OK;
}

/*
 * The backlight on a timer rather than a pin held high, as the board's own BSP
 * does. 1 kHz at ten bits, well above what the eye sees as flicker.
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

#if LCD_SWAP_COLOR_BYTES
static void swap_in_place(void)
{
    uint16_t *p = s_fb;
    for (size_t i = 0, n = (size_t)LCD_W * LCD_H; i < n; i++)
        p[i] = (uint16_t)((p[i] >> 8) | (p[i] << 8));
}

void display_blit(void)
{
    swap_in_place();
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
    swap_in_place();
}
#else
void display_blit(void)
{
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}
#endif

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
