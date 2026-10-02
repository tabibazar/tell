#include "cam.h"

#include <stdio.h>

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cam";
static bool s_flip;

void cam_set_flip(bool flip) { s_flip = flip; }
bool cam_flip(void) { return s_flip; }

static bool cam_up(framesize_t size, int quality)
{
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 39,
        .pin_sccb_sda = 15, .pin_sccb_scl = 16,
        .pin_d7 = 14, .pin_d6 = 13, .pin_d5 = 12, .pin_d4 = 11,
        .pin_d3 = 10, .pin_d2 = 9, .pin_d1 = 8, .pin_d0 = 7,
        .pin_vsync = 42, .pin_href = 41, .pin_pclk = 46,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = size,
        .jpeg_quality = quality,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
        .sccb_i2c_port = 0,
    };
    esp_err_t e = esp_camera_init(&c);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "init: %s", esp_err_to_name(e));
        return false;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, s_flip);
        s->set_hmirror(s, s_flip);
    }
    return true;
}

static void cam_down(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s) s->set_reg(s, 0x3008, 0xff, 0x42);    /* software power-down */
    esp_camera_deinit();
}

void cam_sleep(void)
{
    if (cam_up(FRAMESIZE_QVGA, 12)) cam_down();
}

bool cam_shot(framesize_t size, int quality, const char *path, size_t *bytes)
{
    *bytes = 0;
    int64_t t0 = esp_timer_get_time();
    if (!cam_up(size, quality)) return false;
    /* Exposure and white balance settle over the first second or so of
       frames; keep the last of them. */
    camera_fb_t *fb = NULL;
    int frames = size >= FRAMESIZE_QXGA ? 8 : 12;
    for (int i = 0; i < frames; i++) {
        if (fb) esp_camera_fb_return(fb);
        fb = esp_camera_fb_get();
        vTaskDelay(pdMS_TO_TICKS(size >= FRAMESIZE_QXGA ? 150 : 100));
    }
    bool ok = false;
    if (fb && fb->format == PIXFORMAT_JPEG && fb->len > 1000) {
        FILE *f = fopen(path, "wb");
        if (f) {
            ok = fwrite(fb->buf, 1, fb->len, f) == fb->len;
            ok = (fclose(f) == 0) && ok;
            *bytes = fb->len;
        }
        ESP_LOGI(TAG, "%ux%u, %u bytes -> %s (%lld ms)%s", fb->width, fb->height, (unsigned)fb->len, path,
                 (esp_timer_get_time() - t0) / 1000, ok ? "" : " WRITE FAILED");
    } else {
        ESP_LOGW(TAG, "no frame");
    }
    if (fb) esp_camera_fb_return(fb);
    cam_down();
    return ok;
}
