#include "camera.h"

#if CONFIG_SCREEN_HAVE_CAMERA

#include "i2cbus.h"

#include "esp_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "camera";

static bool s_started = false;

/*
 * Pins and settings the B1 spike proved on real hardware (see main.c's
 * throwaway camera_spike(), removed once B3/B4 land).
 *
 * SCCB decision: SHARED PORT. The OV5640's SCCB lines are GPIO8/7, which are
 * I2CBUS_MAIN (i2cbus.c, I2C_NUM_0) driven by the new i2c_master driver. We
 * set sccb_i2c_port=I2C_NUM_0 and pin_sccb_sda/scl=-1 so esp_camera calls
 * SCCB_Use_Port(): it attaches to the existing bus rather than creating its
 * own, and because it does not own the port, esp_camera_deinit() removes
 * only its own SCCB device and leaves i2cbus's bus alive (confirmed: a
 * DS3231 read worked after deinit). Requires
 * CONFIG_SCCB_HARDWARE_I2C_DRIVER_NEW, set in sdkconfig.defaults.envio.
 *
 * LEDC: the backlight owns LEDC_TIMER_0/CHANNEL_0 (display_axs15231b.c), so
 * the XCLK generator is pushed to TIMER_1/CHANNEL_1 or it would kill the
 * backlight.
 *
 * Switching modes: cam_hal's framebuffer/DMA sizing and jpeg_mode are fixed
 * once, in cam_config() at esp_camera_init() time, and are NOT updated by
 * sensor_t::set_pixformat/set_framesize -- those only write SCCB sensor
 * registers. A runtime sensor switch therefore leaves cam_hal sized for the
 * old format, and esp_camera_fb_get() after it is unreliable. The B1 spike
 * proved the only method that works on this hardware: a full
 * esp_camera_deinit() followed by esp_camera_init() with a config for the
 * new mode. The shared SCCB bus survives that deinit (B1: a DS3231 read
 * worked right after), so this costs nothing extra on the shared-port
 * design above.
 */
static void camera_fill_config(camera_config_t *c)
{
    *c = (camera_config_t){
        .pin_pwdn = -1, .pin_reset = -1,
        .pin_xclk = 38,
        .pin_sccb_sda = -1, .pin_sccb_scl = -1, .sccb_i2c_port = I2C_NUM_0,
        .pin_d0 = 45, .pin_d1 = 47, .pin_d2 = 48, .pin_d3 = 46,
        .pin_d4 = 42, .pin_d5 = 40, .pin_d6 = 39, .pin_d7 = 21,
        .pin_vsync = 17, .pin_href = 18, .pin_pclk = 41,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_1, .ledc_channel = LEDC_CHANNEL_1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
        .fb_count = 1,
        .jpeg_quality = 12,
    };
}

static void camera_fill_preview_config(camera_config_t *c)
{
    camera_fill_config(c);
    c->pixel_format = PIXFORMAT_RGB565;
    c->frame_size = FRAMESIZE_QVGA;
}

static void camera_fill_capture_config(camera_config_t *c)
{
    camera_fill_config(c);
    c->pixel_format = PIXFORMAT_JPEG;
    c->frame_size = FRAMESIZE_SVGA;
}

esp_err_t camera_start(void)
{
    if (s_started) return ESP_OK;

    /* SCCB rides this bus; make sure it exists before esp_camera attaches. */
    esp_err_t err = i2cbus_init(I2CBUS_MAIN);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2cbus_init failed: %s", esp_err_to_name(err));
        return err;
    }

    camera_config_t cfg;
    camera_fill_preview_config(&cfg);

    err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init(RGB565) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_started = true;
    return ESP_OK;
}

void camera_stop(void)
{
    if (!s_started) return;
    esp_camera_deinit();
    s_started = false;
}

bool camera_preview(canvas_t *c)
{
    if (!s_started) return false;

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "camera_preview: fb_get returned NULL");
        return false;
    }

    canvas_blit(c, (const uint16_t *)fb->buf, fb->width, fb->height,
                (c->w - (int)fb->width) / 2, (c->h - (int)fb->height) / 2);
    esp_camera_fb_return(fb);
    return true;
}

/* The OV5640's auto-exposure has not converged right after init or a
   framesize/format change, so the first frame(s) come out dark (B1's
   finding). Grab and discard a few before keeping one. */
static void discard_settling_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb != NULL) esp_camera_fb_return(fb);
    }
}

/* Deinits whatever mode is running and reinits into the preview config, so
   error paths never leave the camera stuck in JPEG mode. Best-effort: if
   this reinit itself fails there is nothing more to do but log it, and
   s_started reflects the true state so a later camera_start() will retry. */
static void fall_back_to_preview(void)
{
    esp_camera_deinit();
    camera_config_t cfg;
    camera_fill_preview_config(&cfg);
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fall_back_to_preview: reinit failed: %s",
                 esp_err_to_name(err));
        s_started = false;
    }
}

esp_err_t camera_capture_jpeg(const uint8_t **out, size_t *len,
                              camera_fb_t **fb_to_return)
{
    if (out == NULL || len == NULL || fb_to_return == NULL)
        return ESP_ERR_INVALID_ARG;
    if (!s_started) return ESP_ERR_INVALID_STATE;

    /* cam_hal's DMA/framebuffer sizing and jpeg_mode are fixed at
       esp_camera_init() and are not updated by a runtime sensor switch
       (see the comment above camera_fill_config), so a mode change is a
       full deinit + reinit with a config for the new mode. */
    esp_camera_deinit();
    camera_config_t cfg;
    camera_fill_capture_config(&cfg);
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera_capture_jpeg: esp_camera_init(JPEG) failed: %s",
                 esp_err_to_name(err));
        fall_back_to_preview();
        return err;
    }

    /* Let auto-exposure settle after the reinit before keeping a frame. */
    discard_settling_frames(3);
    vTaskDelay(pdMS_TO_TICKS(300));

    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGE(TAG, "camera_capture_jpeg: fb_get returned NULL");
        fall_back_to_preview();
        return ESP_FAIL;
    }

    *out = fb->buf;
    *len = fb->len;
    *fb_to_return = fb;
    return ESP_OK;
}

void camera_resume_preview(void)
{
    if (!s_started) return;
    fall_back_to_preview();
}

#endif /* CONFIG_SCREEN_HAVE_CAMERA */
