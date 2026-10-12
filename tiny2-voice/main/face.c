#include "face.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aafont.h"
#include "canvas.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "face";

#define W 240
#define H 240
#define PIN_SCL 39
#define PIN_SDA 40
#define PIN_CS  41
#define PIN_DC  42
#define PIN_RST 38
#define STRIP   40                       /* rows sent at a time */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;                   /* PSRAM: the frame, native RGB565 */
static uint16_t *s_dma[2];               /* internal: strips, byte-swapped for the panel */
static float (*s_level)(void);

/* An ellipse, filled, row by row. */
static void ellipse(canvas_t *c, int cx, int cy, int rx, int ry, uint16_t col)
{
    if (rx < 1 || ry < 1) return;
    for (int dy = -ry; dy <= ry; dy++) {
        float f = 1.0f - (float)(dy * dy) / (float)(ry * ry);
        int half = (int)(rx * sqrtf(f > 0 ? f : 0) + 0.5f);
        canvas_fill_rect(c, cx - half, cy + dy, 2 * half + 1, 1, col);
    }
}

typedef struct { float lx, ly, tx, ty; int64_t next_look, next_blink, blink_t0; float mouth; } face_state_t;

static void draw(canvas_t *c, face_state_t *st, int64_t now)
{
    const uint16_t bg = RGB(12, 14, 24), white = RGB(240, 242, 248), pupil = RGB(20, 24, 40);
    canvas_fill_rect(c, 0, 0, W, H, bg);

    /* Glances: every few seconds a new place to look, eased towards. */
    if (now > st->next_look) {
        st->tx = (float)((int)(esp_random() % 21) - 10);
        st->ty = (float)((int)(esp_random() % 13) - 6);
        if (esp_random() % 3 == 0) st->tx = st->ty = 0;
        st->next_look = now + 1500000 + (esp_random() % 3000000);
    }
    st->lx += (st->tx - st->lx) * 0.25f;
    st->ly += (st->ty - st->ly) * 0.25f;
    /* Blinks: 150 ms, every 2-6 s. */
    if (now > st->next_blink) { st->blink_t0 = now; st->next_blink = now + 2000000 + (esp_random() % 4000000); }
    float b = (float)(now - st->blink_t0) / 150000.0f;
    float open = b >= 1.0f ? 1.0f : fabsf(1.0f - 2.0f * b);
    int ry = (int)(30 * open);
    if (ry < 3) ry = 3;
    for (int e = 0; e < 2; e++) {
        int cx = e ? 160 : 80, cy = 98;
        ellipse(c, cx, cy, 28, ry, white);
        if (ry > 10) {
            ellipse(c, cx + (int)st->lx, cy + (int)st->ly, 13, ry * 13 / 30 + 1, pupil);
            canvas_disc(c, cx + (int)st->lx - 4, cy + (int)st->ly - 5, 3, white);   /* a glint */
        }
    }

    /* The mouth: open with the voice, a smile when quiet. */
    float lv = s_level ? s_level() : -1.0f;
    float want = lv < 0 ? 0 : lv;
    st->mouth += (want - st->mouth) * (want > st->mouth ? 0.7f : 0.35f);
    if (lv < 0 && st->mouth < 0.05f) {
        for (int i = 0; i < 4; i++) {                /* a smile: the bottom of a ring */
            for (int x = -34; x <= 34; x++) {
                int y = (int)(sqrtf(40.0f * 40.0f - x * x) * 0.45f);
                canvas_fill_rect(c, 120 + x, 150 + y + i - 2, 1, 1, white);
            }
        }
    } else {
        int h = 4 + (int)(st->mouth * 36);
        ellipse(c, 120, 168, 32, h, RGB(90, 20, 30));
        if (h > 12) ellipse(c, 120, 168 + h / 2, 18, h / 3, RGB(220, 90, 110));    /* the tongue */
    }

    time_t t = time(NULL);
    if (t > 1767225600) {                             /* the clock is set */
        struct tm lt;
        localtime_r(&t, &lt);
        char s[8];
        snprintf(s, sizeof s, "%d:%02d", lt.tm_hour, lt.tm_min);
        aafont_draw(c, &aafont_inter_label, W / 2, 206, s, RGB(110, 118, 140), AAFONT_CENTRE);
    }
}

static void push(void)
{
    for (int y = 0, k = 0; y < H; y += STRIP, k ^= 1) {
        uint16_t *d = s_dma[k];
        const uint16_t *s = s_fb + y * W;
        for (int i = 0; i < W * STRIP; i++) d[i] = (uint16_t)(s[i] << 8 | s[i] >> 8);   /* the panel wants big-endian */
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, W, y + STRIP, d);
    }
}

static void face_task(void *arg)
{
    canvas_t c;
    canvas_init(&c, s_fb, W, H, 1);
    face_state_t st = { 0 };
    for (;;) {
        int64_t now = esp_timer_get_time();
        draw(&c, &st, now);
        push();
        int64_t spent = (esp_timer_get_time() - now) / 1000;
        vTaskDelay(pdMS_TO_TICKS(spent < 40 ? 40 - spent : 1));      /* ~25 frames a second */
    }
}

esp_err_t face_init(float (*level_fn)(void))
{
    s_level = level_fn;
    spi_bus_config_t bus = { .sclk_io_num = PIN_SCL, .mosi_io_num = PIN_SDA, .miso_io_num = -1,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = W * STRIP * 2 };
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) return e;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t ioc = { .dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS, .pclk_hz = 40 * 1000 * 1000,
                                          .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4 };
    if ((e = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &io)) != ESP_OK) return e;
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16 };
    if ((e = esp_lcd_new_panel_gc9a01(io, &pc, &s_panel)) != ESP_OK) return e;
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);
    esp_lcd_panel_disp_on_off(s_panel, true);
    s_fb = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) s_dma[i] = heap_caps_malloc(W * STRIP * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_fb || !s_dma[0] || !s_dma[1]) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(face_task, "face", 4096, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "GC9A01 up");
    return ESP_OK;
}
