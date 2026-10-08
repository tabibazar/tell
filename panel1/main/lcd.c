/*
 * panel1's display: the Waveshare ESP32-S3-Touch-LCD-4B's 480x480 ST7701.
 *
 * Pins, timings and the init table are Waveshare's own BSP
 * (waveshare/esp32_s3_touch_lcd_4b 2.0.0, what the factory image runs),
 * cross-checked with the schematic:
 *
 *   I2C        SDA 47, SCL 48: TCA9554 0x20, GT911 0x5D, AXP2101 0x34, ...
 *   TCA9554    EXIO0 CS, EXIO1 SDA, EXIO2 SCK (the ST7701's 3-wire SPI),
 *              EXIO3 amplifier enable, EXIO4 power key (in),
 *              EXIO5 touch RST, EXIO6 touch INT, EXIO7 LCD RST
 *   RGB        PCLK 9, HSYNC 46, VSYNC 3, DE 17; B 40 41 42 2 1,
 *              G 21 8 18 45 38 39, R 10 11 12 13 14
 *   Backlight  GPIO4, INVERTED: it feeds the boost's feedback node, so 0 %
 *              duty is full brightness and a floating pin is on.
 *
 * The ST7701 wants its setup over 9-bit SPI whose three lines are expander
 * outputs, so every clock edge is an I2C write: ~0.3 s for the whole table,
 * once, at boot.
 */
#include "lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define W 480
#define H 480

#define PIN_SDA  47
#define PIN_SCL  48
#define PIN_BL   4

#define TCA_ADDR 0x20
#define TCA_OUT  0x01
#define TCA_CFG  0x03   /* 1 = input */

#define X_CS     (1 << 0)
#define X_SDA    (1 << 1)
#define X_SCK    (1 << 2)
#define X_AMP    (1 << 3)
#define X_KEY    (1 << 4)
#define X_TRST   (1 << 5)
#define X_TINT   (1 << 6)
#define X_LRST   (1 << 7)

static const char *TAG = "lcd";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tca;
static uint8_t s_out;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_draw;
static canvas_t s_canvas;

static esp_err_t tca_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_tca, b, 2, 50);
}

static void tca_set(uint8_t bits, bool on)
{
    s_out = on ? (s_out | bits) : (s_out & ~bits);
    tca_write(TCA_OUT, s_out);
}

#define TCA_IN   0x00
/* PWR pulls EXIO4 low while it is held: the AXP2101's PWRON line, pulled up.
   Checked on the board; flip this if the log's "PWR key" lines say otherwise. */
#define KEY_DOWN 0

bool lcd_pwr_key(void)
{
    uint8_t reg = TCA_IN, v = 0;
    if (i2c_master_transmit_receive(s_tca, &reg, 1, &v, 1, 50) != ESP_OK) return false;
    return ((v & X_KEY) ? 1 : 0) == KEY_DOWN;
}

/* One 9-bit frame: D/C, then the byte, MSB first, latched on SCK rising. */
static void spi9(bool data, uint8_t v)
{
    uint16_t word = (data ? 0x100 : 0) | v;
    for (int i = 8; i >= 0; i--) {
        uint8_t o = s_out & ~(X_SCK | X_SDA);
        if (word & (1 << i)) o |= X_SDA;
        tca_write(TCA_OUT, o);              /* SCK low, data set */
        tca_write(TCA_OUT, o | X_SCK);      /* rising edge */
        s_out = o | X_SCK;
    }
}

static void st_cmd(uint8_t cmd, const uint8_t *data, int len)
{
    tca_set(X_CS, false);
    spi9(false, cmd);
    for (int i = 0; i < len; i++) spi9(true, data[i]);
    tca_set(X_CS, true);
}

typedef struct { uint8_t cmd; const uint8_t *data; uint8_t len; uint16_t delay_ms; } st7701_cmd_t;

/* Waveshare's lcd_init_cmds[] for the 4B, verbatim. */
static const st7701_cmd_t INIT[] = {
    {0x11, NULL, 0, 120},
    {0xFF, (const uint8_t[]){0x77,0x01,0x00,0x00,0x10}, 5, 0},
    {0xC0, (const uint8_t[]){0x3B,0x00}, 2, 0},
    {0xC1, (const uint8_t[]){0x0D,0x02}, 2, 0},
    {0xC2, (const uint8_t[]){0x21,0x08}, 2, 0},
    {0xCD, (const uint8_t[]){0x08}, 1, 0},
    {0xB0, (const uint8_t[]){0x00,0x11,0x18,0x0E,0x11,0x06,0x07,0x08,0x07,0x22,0x04,0x12,0x0F,0xAA,0x31,0x18}, 16, 0},
    {0xB1, (const uint8_t[]){0x00,0x11,0x19,0x0E,0x12,0x07,0x08,0x08,0x08,0x22,0x04,0x11,0x11,0xA9,0x32,0x18}, 16, 0},
    {0xFF, (const uint8_t[]){0x77,0x01,0x00,0x00,0x11}, 5, 0},
    {0xB0, (const uint8_t[]){0x60}, 1, 0},
    {0xB1, (const uint8_t[]){0x30}, 1, 0},
    {0xB2, (const uint8_t[]){0x87}, 1, 0},
    {0xB3, (const uint8_t[]){0x80}, 1, 0},
    {0xB5, (const uint8_t[]){0x49}, 1, 0},
    {0xB7, (const uint8_t[]){0x85}, 1, 0},
    {0xB8, (const uint8_t[]){0x21}, 1, 0},
    {0xC1, (const uint8_t[]){0x78}, 1, 0},
    {0xC2, (const uint8_t[]){0x78}, 1, 20},
    {0xE0, (const uint8_t[]){0x00,0x1B,0x02}, 3, 0},
    {0xE1, (const uint8_t[]){0x08,0xA0,0x00,0x00,0x07,0xA0,0x00,0x00,0x00,0x44,0x44}, 11, 0},
    {0xE2, (const uint8_t[]){0x11,0x11,0x44,0x44,0xED,0xA0,0x00,0x00,0xEC,0xA0,0x00,0x00}, 12, 0},
    {0xE3, (const uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0xE4, (const uint8_t[]){0x44,0x44}, 2, 0},
    {0xE5, (const uint8_t[]){0x0A,0xE9,0xD8,0xA0,0x0C,0xEB,0xD8,0xA0,0x0E,0xED,0xD8,0xA0,0x10,0xEF,0xD8,0xA0}, 16, 0},
    {0xE6, (const uint8_t[]){0x00,0x00,0x11,0x11}, 4, 0},
    {0xE7, (const uint8_t[]){0x44,0x44}, 2, 0},
    {0xE8, (const uint8_t[]){0x09,0xE8,0xD8,0xA0,0x0B,0xEA,0xD8,0xA0,0x0D,0xEC,0xD8,0xA0,0x0F,0xEE,0xD8,0xA0}, 16, 0},
    {0xEB, (const uint8_t[]){0x02,0x00,0xE4,0xE4,0x88,0x00,0x40}, 7, 0},
    {0xEC, (const uint8_t[]){0x3C,0x00}, 2, 0},
    {0xED, (const uint8_t[]){0xAB,0x89,0x76,0x54,0x02,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x20,0x45,0x67,0x98,0xBA}, 16, 0},
    {0xFF, (const uint8_t[]){0x77,0x01,0x00,0x00,0x00}, 5, 0},
    {0x36, (const uint8_t[]){0x00}, 1, 0},
    {0x3A, (const uint8_t[]){0x66}, 1, 0},
    {0x21, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

i2c_master_bus_handle_t lcd_i2c(void) { return s_bus; }

void lcd_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 1023 * (100 - percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

esp_err_t lcd_init(void)
{
    /* Backlight first, dark, so the panel's power-on noise is not seen. */
    ledc_timer_config_t lt = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&lt));
    ledc_channel_config_t lc = {
        .gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1, .duty = 1023,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&lc));

    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_bus));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TCA_ADDR, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dc, &s_tca));

    /* CS high, clock and data low, amplifier off, both resets held. */
    s_out = X_CS;
    esp_err_t e = tca_write(TCA_OUT, s_out);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 at 0x%02X not answering: %s", TCA_ADDR, esp_err_to_name(e));
        return e;
    }
    tca_write(TCA_CFG, X_KEY);   /* everything an output but the power key */

    /* The touch chip's reset, INT held low throughout: that picks 0x5D. */
    tca_set(X_TINT, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    tca_set(X_TRST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    tca_set(X_TRST, true);
    vTaskDelay(pdMS_TO_TICKS(60));
    tca_write(TCA_CFG, X_KEY | X_TINT);   /* INT back to the GT911 */

    /* The panel's reset, then its setup. */
    tca_set(X_LRST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    tca_set(X_LRST, true);
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; i < sizeof INIT / sizeof INIT[0]; i++) {
        st_cmd(INIT[i].cmd, INIT[i].data, INIT[i].len);
        if (INIT[i].delay_ms) vTaskDelay(pdMS_TO_TICKS(INIT[i].delay_ms));
    }

    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = W * 20,
        .psram_trans_align = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 17,
        .pclk_gpio_num = 9,
        .disp_gpio_num = -1,
        .data_gpio_nums = { 40, 41, 42, 2, 1, 21, 8, 18, 45, 38, 39, 10, 11, 12, 13, 14 },
        .timings = {
            .pclk_hz = 16 * 1000 * 1000,
            .h_res = W,
            .v_res = H,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 10,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 10,
            .vsync_back_porch = 10,
            .vsync_front_porch = 10,
        },
        .flags.fb_in_psram = 1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));

    s_draw = heap_caps_aligned_calloc(64, W * H, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_draw) return ESP_ERR_NO_MEM;
    canvas_init(&s_canvas, s_draw, W, H, 1);
    lcd_show();
    lcd_backlight(100);
    ESP_LOGI(TAG, "480x480 up");
    return ESP_OK;
}

canvas_t *lcd_canvas(void) { return &s_canvas; }

void lcd_show(void)
{
    /* Copies the whole page into the frame the panel streams from. */
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, W, H, s_draw);
}
