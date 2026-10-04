/*
 * tiny2: an OV7670 on a DevKitC-1 clone. First picture.
 *
 * The wiring as found by probing (2026-10-04; the OV7670 has no fixed pins
 * to the ESP32, and these were mapped from its signals and its walking-one
 * test pattern, see git history):
 *   SIOD 5, SIOC 6 (SCCB, 0x21: PID 0x76 VER 0x73)
 *   XCLK 9, PCLK 16, HREF 46, VSYNC 14
 *   D0 13, D1 3, D2 12, D3 -- not arriving (GPIO21 stands in, pulled down),
 *   D4 11, D5 18, D6 10, D7 17
 *   RESET and PWDN left to the module's own pull-ups/downs
 * A QVGA RGB565 frame, compressed to JPEG here and sent over the console
 * between JPGBEGIN/JPGEND for the Mac to save.
 */
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "img_converters.h"
#include "mbedtls/base64.h"

static const char *TAG = "tiny2";

void app_main(void)
{
    gpio_config_t d3 = { .pin_bit_mask = 1ULL << 21, .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&d3);
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 9,
        .pin_sccb_sda = 5, .pin_sccb_scl = 6,
        .pin_d7 = 17, .pin_d6 = 10, .pin_d5 = 18, .pin_d4 = 11,
        .pin_d3 = 21, .pin_d2 = 12, .pin_d1 = 3, .pin_d0 = 13,
        .pin_vsync = 14, .pin_href = 46, .pin_pclk = 16,
        .xclk_freq_hz = 10000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565, .frame_size = FRAMESIZE_QVGA,
        .fb_count = 1, .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t e = esp_camera_init(&c);
    if (e != ESP_OK) { ESP_LOGE(TAG, "camera init: %s", esp_err_to_name(e)); return; }
    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "camera up: PID 0x%02X", s ? s->id.PID : 0);
    for (;;) {
        camera_fb_t *fb = NULL;
        for (int i = 0; i < 6; i++) {               /* exposure settles */
            if (fb) esp_camera_fb_return(fb);
            fb = esp_camera_fb_get();
        }
        if (!fb) { ESP_LOGW(TAG, "no frame"); vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
        uint8_t *jpg = NULL;
        size_t jl = 0;
        bool ok = frame2jpg(fb, 80, &jpg, &jl);
        ESP_LOGI(TAG, "frame %ux%u, %u bytes raw -> JPEG %s %u bytes", fb->width, fb->height, (unsigned)fb->len,
                 ok ? "ok" : "FAILED", (unsigned)jl);
        esp_camera_fb_return(fb);
        if (ok) {
            printf("JPGBEGIN %u\n", (unsigned)jl);
            static unsigned char line[1100];
            size_t ol;
            for (size_t off = 0; off < jl; off += 768) {
                size_t n = jl - off < 768 ? jl - off : 768;
                mbedtls_base64_encode(line, sizeof line, &ol, jpg + off, n);
                line[ol] = 0;
                printf("%s\n", line);
            }
            printf("JPGEND\n");
            fflush(stdout);
            free(jpg);
        }
        vTaskDelay(pdMS_TO_TICKS(20000));
    }
}
