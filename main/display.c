#include "display.h"

#include "textwrap.h"
#include "font.h"

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
_Static_assert(LCD_W / FONT_W >= DISPLAY_COLS, "font too wide for DISPLAY_COLS");
_Static_assert(LCD_H / FONT_H >= DISPLAY_ROWS, "font too tall for DISPLAY_ROWS");

/* Centre the text block in the leftover pixels. Without this the first row
   starts at y=0, where a small panel-offset error clips the tops of glyphs. */
#define MARGIN_X ((LCD_W - DISPLAY_COLS * FONT_W) / 2)
#define MARGIN_Y ((LCD_H - DISPLAY_ROWS * FONT_H) / 2)

/* 0xFFFF and 0x0000 are byte-order agnostic, so white-on-black needs no swap. */
#define COLOR_FG 0xFFFF
#define COLOR_BG 0x0000

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;

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

    display_show_text(NULL);
    ESP_LOGI(TAG, "panel up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, DISPLAY_COLS, DISPLAY_ROWS);
    return ESP_OK;
}

/* Draws one glyph at an absolute pixel position, scaled by an integer factor.
   Scaling by pixel replication keeps the font table single-source. */
static void draw_glyph_at(char ch, int ox, int oy, int scale)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t *glyph = font_glyphs[(unsigned char)ch - FONT_FIRST];

    for (int y = 0; y < FONT_H; y++) {
        /* Each row is FONT_STRIDE bytes, big-endian, pixel 0 the highest bit. */
        uint32_t bits = 0;
        for (int b = 0; b < FONT_STRIDE; b++)
            bits = (bits << 8) | glyph[y * FONT_STRIDE + b];
        if (bits == 0) continue;

        for (int x = 0; x < FONT_W; x++) {
            if (!((bits >> (FONT_W - 1 - x)) & 1)) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = oy + y * scale + sy;
                if (py < 0 || py >= LCD_H) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = ox + x * scale + sx;
                    if (px >= 0 && px < LCD_W) s_fb[py * LCD_W + px] = COLOR_FG;
                }
            }
        }
    }
}

static void clear_fb(void)
{
    for (int i = 0; i < LCD_W * LCD_H; i++) s_fb[i] = COLOR_BG;
}

void display_show_text(const char *utf8)
{
    if (s_fb == NULL) return;

    char lines[DISPLAY_ROWS][TW_MAX_COLS + 1];
    size_t n = textwrap(utf8, DISPLAY_COLS, DISPLAY_ROWS, lines);

    /* Render off-screen, then blit once: no partial frames, no flicker. */
    clear_fb();
    for (size_t row = 0; row < n; row++)
        for (size_t col = 0; lines[row][col] != '\0'; col++)
            draw_glyph_at(lines[row][col],
                          MARGIN_X + (int)col * FONT_W,
                          MARGIN_Y + (int)row * FONT_H, 1);

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}

void display_show_big(const char *text)
{
    if (s_fb == NULL || text == NULL) return;

    int len = 0;
    while (text[len] != '\0' && len < DISPLAY_COLS) len++;

    /* Largest whole-integer scale that still fits, so the clock fills the panel. */
    int scale = 1;
    while ((scale + 1) * FONT_W * len <= LCD_W && (scale + 1) * FONT_H <= LCD_H)
        scale++;

    int ox = (LCD_W - len * FONT_W * scale) / 2;
    int oy = (LCD_H - FONT_H * scale) / 2;

    clear_fb();
    for (int i = 0; i < len; i++)
        draw_glyph_at(text[i], ox + i * FONT_W * scale, oy, scale);

    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
}
