/*
 * envio (Waveshare ESP32-S3-Touch-LCD-3.5B): AXS15231B 320x480 over QSPI.
 *
 * A third panel bus for this tree -- neither lilly's 8-bit i80 nor the ST7789
 * SPI the returned boards used. The controller is one part that is both the
 * display (QSPI) and the touch (I2C); only the display half is driven here.
 *
 * The pins, the 40 MHz clock and the long vendor init table are Waveshare's
 * own, from their factory firmware (ESP-IDF/01_factory bsp_display), which is
 * what was on the board when it arrived. The init table is what the panel
 * needs to show anything but noise, so it is carried verbatim rather than
 * trusting the driver's built-in defaults.
 *
 * The panel rail is behind the AXP2101, not a GPIO, so axp2101_init runs first
 * -- see axp2101.h. The backlight, though, is a plain LEDC pin like lilly's.
 */
#include "display.h"

#include "axp2101.h"
#include "canvas.h"
#include "i2cbus.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>


/* Waveshare ESP32-S3-Touch-LCD-3.5B, from their bsp_display.h. */
#define PIN_LCD_CS     12
#define PIN_LCD_SCLK    5
#define PIN_LCD_D0      1
#define PIN_LCD_D1      2
#define PIN_LCD_D2      3
#define PIN_LCD_D3      4
#define PIN_LCD_BL      6
/* The panel has no reset line brought out -- the demo passes RST = -1 and the
   framework issues the software reset over the bus. */
#define PIN_LCD_RST    -1

/*
 * Native portrait, 320x480 -- the board stands upright. A software landscape
 * rotation was tried (draw a 480x320 canvas, transpose into a portrait panel
 * buffer) but this QSPI panel would only ever refresh part of the height, so
 * it was abandoned for the orientation the panel is actually built for.
 */
#define LCD_W 320
#define LCD_H 480

/*
 * The framebuffer lives in PSRAM (307 KB, far too big for internal RAM), and
 * SPI DMA cannot read PSRAM directly -- the master bounces each transfer
 * through an internal DMA buffer it sizes to that transfer. A whole-frame
 * transfer would need a 307 KB internal bounce that does not exist (~235 KB
 * internal all told), which fails as "could not allocate priv TX buffer" and
 * every blit comes back ESP_ERR_NO_MEM. So a single transfer is capped at a
 * band of rows; esp_lcd splits the frame into a handful of them, each bounced
 * through a buffer small enough to fit. Waveshare's own demo flushes in the
 * same ~15 KB unit. 24 rows is 320*24*2 = 15 KB.
 */
#define LCD_BAND_ROWS 24

/* Waveshare drive the bus at 40 MHz. */
#define LCD_PCLK_HZ (40 * 1000 * 1000)

#define LCD_HOST SPI2_HOST

/*
 * The AXS15231B initialisation, copied verbatim from Waveshare's factory
 * firmware. Without it the panel shows only noise -- a documented failure of
 * this exact board when brought up on the driver's stock sequence. Do not
 * trim it without something on the glass to check against.
 */
static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t[]){0x00}, 0, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

/*
 * The panel's reset line is not a GPIO -- it hangs off the TCA9554 I2C
 * expander at EXIO1, which is why the panel_dev_config passes RST = -1 and
 * esp_lcd_panel_reset only issues the software reset over the bus. The glass
 * needs the hardware pulse as well: without it the controller takes commands
 * and the blit reports success, but nothing lights. So pulse EXIO1 low-high
 * before init, with Waveshare's own 100 ms / 200 ms timing.
 *
 * Read-modify-write so the expander's other pins are left alone -- the PWR
 * button sits on this same chip.
 */
#define TCA9554_ADDR   0x20
#define TCA9554_OUTPUT 0x01
#define TCA9554_CONFIG 0x03
#define PANEL_RST_BIT  0x02   /* EXIO1 */

static esp_err_t tca_wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, sizeof b, 50);
}

static esp_err_t panel_hw_reset(void)
{
    i2c_master_dev_handle_t dev;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(i2cbus_handle(I2CBUS_MAIN), &cfg, &dev) != ESP_OK)
        return ESP_ERR_NOT_FOUND;

    uint8_t reg, cur;
    reg = TCA9554_CONFIG;
    if (i2c_master_transmit_receive(dev, &reg, 1, &cur, 1, 50) == ESP_OK)
        tca_wr(dev, TCA9554_CONFIG, (uint8_t)(cur & ~PANEL_RST_BIT)); /* EXIO1 output */

    reg = TCA9554_OUTPUT;
    if (i2c_master_transmit_receive(dev, &reg, 1, &cur, 1, 50) == ESP_OK)
        tca_wr(dev, TCA9554_OUTPUT, (uint8_t)(cur & ~PANEL_RST_BIT)); /* assert low */
    vTaskDelay(pdMS_TO_TICKS(100));
    reg = TCA9554_OUTPUT;
    if (i2c_master_transmit_receive(dev, &reg, 1, &cur, 1, 50) == ESP_OK)
        tca_wr(dev, TCA9554_OUTPUT, (uint8_t)(cur | PANEL_RST_BIT));  /* release high */
    vTaskDelay(pdMS_TO_TICKS(200));

    i2c_master_bus_rm_device(dev);
    return ESP_OK;
}

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static canvas_t s_canvas;

static void backlight_init(void);

esp_err_t display_init(void)
{
    /* The panel rail comes out of the PMIC, not a GPIO. Bring it up first, or
       the board looks like a dead port when it is really an unlit screen. */
    esp_err_t err = axp2101_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMIC did not come up; the panel has no power");
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    /* The rails are up; now pulse the panel's reset, which lives on the I2C
       expander, before speaking QSPI to the controller. */
    if (panel_hw_reset() != ESP_OK)
        ESP_LOGW(TAG, "expander reset failed; the panel may stay dark");

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_LCD_SCLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        /* Capped at one band, not the whole frame -- see LCD_BAND_ROWS.
           display_blit still issues one draw; esp_lcd splits it to this. */
        .max_transfer_sz = LCD_W * LCD_BAND_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg =
        AXS15231B_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    /*
     * The framebuffer is in PSRAM, so each queued transfer needs an internal
     * DMA bounce buffer. The component's default queue depth is 10, and ten
     * bounces of a 24-row band (~15 KB each) is ~150 KB of internal RAM at
     * once -- more than is free beside NimBLE, so the later chunks fail
     * silently and the bottom of the frame is never written ("bottom half
     * blank"). A shallow queue keeps only a few bounces in flight and lets
     * them recycle, so the whole frame gets through. A near-static dashboard
     * does not need the pipelining.
     */
    io_cfg.trans_queue_depth = 3;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(io_handle, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* Native portrait; the landscape is done in software at blit time. No
       swap_xy here -- it is broken for this QSPI driver (see the dimensions
       note above). */

    /*
     * Set the full row window once. This QSPI driver's draw_bitmap sends the
     * column window (CASET) on every draw but never the row window (RASET), so
     * RASET stays at whatever the panel powered up with -- a partial range, so
     * only part of the height ever fills and the rest of the panel is left
     * frozen ("only half the screen refreshes"). 0x2B with 0x0000..0x01DF is
     * rows 0..479, the full height; draw_bitmap keeps setting CASET, so only
     * this is needed.
     */
    esp_lcd_panel_io_tx_param(io_handle, 0x2B,
                              (uint8_t[]){ 0x00, 0x00, 0x01, 0xDF }, 4);
    /*
     * false, not true, turns the display ON here. The component's disp_on_off
     * is implemented as panel_axs15231b_disp_off(panel, bool off): the flag is
     * "off", inverted from esp_lcd's usual "on_off", so true would issue
     * DISPOFF. Waveshare's own demo passes false for the same reason. Our init
     * table has no 0x29, so this call is the only thing that lights the panel
     * -- getting it backwards is exactly why the first flashes were blank.
     */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, false));

    /* 320*480*2 = 307 KB does not fit internal DMA memory beside NimBLE, so the
       framebuffer goes to PSRAM. envio has 8 MB octal; the build turns it on. */
    s_fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for framebuffer");
        return ESP_ERR_NO_MEM;
    }
    canvas_init(&s_canvas, s_fb, LCD_W, LCD_H, 1);

    backlight_init();
    display_set_brightness(CONFIG_SCREEN_BRIGHTNESS);

    display_show_text(NULL);
    ESP_LOGI(TAG, "QSPI AXS15231B up: %dx%d, %d cols x %d rows",
             LCD_W, LCD_H, s_canvas.cols, s_canvas.rows);
    return ESP_OK;
}

/* Backlight on a 1 kHz 10-bit LEDC timer, as lilly's is. */
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
        .gpio_num = PIN_LCD_BL,
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
    /* esp_lcd splits this full-frame draw into bus-sized chunks itself
       (esp_lcd_panel_io_spi tx_color), so one call is right. Log a failure
       once rather than per frame: a blit that silently fails looks like a
       dead panel, the very thing hardest to tell apart from an unlit rail. */
    static bool warned;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb);
    if (err != ESP_OK && !warned) {
        warned = true;
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
    }
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
