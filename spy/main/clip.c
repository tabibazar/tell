#include "clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_h264_enc_single_sw.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "mp4mux.h"
#include "stamp.h"

static const char *TAG = "clip";

static uint8_t *read_file(const char *path, size_t *len)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) return NULL;
    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    FILE *f = fopen(path, "rb");
    if (!buf || !f) { free(buf); if (f) fclose(f); return NULL; }
    *len = fread(buf, 1, st.st_size, f);
    fclose(f);
    return buf;
}

/* RGB888 -> I420, BT.601 limited range (what a player assumes of H.264 that
   says nothing about it). Chroma from each 2x2 block's average. */
static void rgb_to_i420(const uint8_t *rgb, uint8_t *yuv)
{
    uint8_t *Y = yuv, *U = yuv + CLIP_W * CLIP_H, *V = U + CLIP_W * CLIP_H / 4;
    for (int y = 0; y < CLIP_H; y += 2) {
        for (int x = 0; x < CLIP_W; x += 2) {
            int rs = 0, gs = 0, bs = 0;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    const uint8_t *p = rgb + ((y + dy) * CLIP_W + x + dx) * 3;
                    int r = p[0], g = p[1], b = p[2];
                    Y[(y + dy) * CLIP_W + x + dx] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    rs += r; gs += g; bs += b;
                }
            }
            rs >>= 2; gs >>= 2; bs >>= 2;
            U[(y / 2) * (CLIP_W / 2) + x / 2] = (uint8_t)(((-38 * rs - 74 * gs + 112 * bs + 128) >> 8) + 128);
            V[(y / 2) * (CLIP_W / 2) + x / 2] = (uint8_t)(((112 * rs - 94 * gs - 18 * bs + 128) >> 8) + 128);
        }
    }
}

bool clip_make(const char *day_dir, int first_hour, int last_hour, int last_min, const char *out, int *frames)
{
    *frames = 0;
    const size_t yuv_len = CLIP_W * CLIP_H * 3 / 2;
    uint8_t *rgb = heap_caps_malloc(CLIP_W * CLIP_H * 3, MALLOC_CAP_SPIRAM);
    uint8_t *yuv = heap_caps_aligned_alloc(16, yuv_len, MALLOC_CAP_SPIRAM);
    uint8_t *enc_out = heap_caps_malloc(yuv_len, MALLOC_CAP_SPIRAM);
    char *iobuf = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    FILE *f = NULL;
    esp_h264_enc_handle_t enc = NULL;
    mp4mux_t *mux = NULL;
    bool ok = false;
    if (!rgb || !yuv || !enc_out || !iobuf) goto done;

    esp_h264_enc_cfg_sw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_I420, .gop = CLIP_FPS * 3, .fps = CLIP_FPS,
        .res = { .width = CLIP_W, .height = CLIP_H },
        /* 800 kbit/s: a day's minute of video is ~6 MB, three minutes up. */
        .rc = { .bitrate = 800000, .qp_min = 22, .qp_max = 38 },
    };
    if (esp_h264_enc_sw_new(&cfg, &enc) != ESP_H264_ERR_OK || esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "encoder would not open");
        goto done;
    }
    f = fopen(out, "wb+");
    if (!f) { ESP_LOGE(TAG, "cannot write %s", out); goto done; }
    setvbuf(f, iobuf, _IOFBF, 32 * 1024);
    mux = mp4mux_open(f, CLIP_W, CLIP_H, CLIP_FPS);
    if (!mux) goto done;

    int64_t t0 = esp_timer_get_time();
    for (int fm = first_hour * 60; fm <= last_hour * 60 + last_min && fm < 24 * 60; fm++) {
        int hour = fm / 60, m = fm % 60;
        char path[64];
        snprintf(path, sizeof path, "%s/%02d%02d.jpg", day_dir, hour, m);
        size_t jlen = 0;
        uint8_t *jpg = read_file(path, &jlen);
        if (!jpg) continue;
        esp_jpeg_image_cfg_t jc = {
            .indata = jpg, .indata_size = jlen,
            .outbuf = rgb, .outbuf_size = CLIP_W * CLIP_H * 3,
            .out_format = JPEG_IMAGE_FORMAT_RGB888, .out_scale = JPEG_IMAGE_SCALE_0,
        };
        esp_jpeg_image_output_t jo;
        esp_err_t je = esp_jpeg_decode(&jc, &jo);
        free(jpg);
        if (je != ESP_OK || jo.width != CLIP_W || jo.height != CLIP_H) {
            ESP_LOGW(TAG, "%s: not a %dx%d JPEG (%s)", path, CLIP_W, CLIP_H, esp_err_to_name(je));
            continue;
        }
        rgb_to_i420(rgb, yuv);
        stamp_time(yuv, CLIP_W, CLIP_H, hour, m);
        esp_h264_enc_in_frame_t in = { .raw_data = { .buffer = yuv, .len = yuv_len }, .pts = *frames * 1000 / CLIP_FPS };
        esp_h264_enc_out_frame_t of = { .raw_data = { .buffer = enc_out, .len = yuv_len } };
        if (esp_h264_enc_process(enc, &in, &of) != ESP_H264_ERR_OK) {
            ESP_LOGE(TAG, "encode failed at %s", path);
            break;
        }
        bool key = of.frame_type == ESP_H264_FRAME_TYPE_IDR || of.frame_type == ESP_H264_FRAME_TYPE_I;
        if (!mp4mux_add(mux, enc_out, of.length, key)) {
            ESP_LOGE(TAG, "mux failed at %s", path);
            break;
        }
        (*frames)++;
    }
    ok = mp4mux_close(mux) && *frames > 0;
    mux = NULL;
    ESP_LOGI(TAG, "%s: %d frames in %lld s%s", out, *frames, (esp_timer_get_time() - t0) / 1000000,
             ok ? "" : ", FAILED");
done:
    if (mux) mp4mux_close(mux);
    if (f) fclose(f);
    if (!ok) remove(out);
    if (enc) { esp_h264_enc_close(enc); esp_h264_enc_del(enc); }
    free(rgb);
    free(yuv);
    free(enc_out);
    free(iobuf);
    return ok;
}
