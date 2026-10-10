#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lcd.h"

static const char *TAG = "audio";

#define PIN_MCLK 5
#define PIN_BCLK 16
#define PIN_WS   7
#define PIN_DOUT 6
#define PIN_DIN  15
/*
 * The ES7210's four TDM slots, measured on this board with the console's
 * `slots` (quiet / beep, RMS): 0 = MIC1 (24 / 105), 1 = the speaker's drive
 * looped back, the reference (1 / 1020, a clean sine), 2 = MIC2 (24 / 90),
 * 3 = nothing (1 / 1). The same order as the AUDIO-Board's (speaker_app.c).
 * Recording takes MIC1 alone: averaging in MIC2 could comb-filter speech
 * between two mics centimetres apart, and MIC1 reads the louder of the two.
 */
#define SLOTS    4
#define SLOT_MIC 0
#define FRAME    320        /* 20 ms at 16 kHz; one read is one DMA buffer */
#define DMA_BUFS 6
#define MIC_GAIN_DB 30.0f

static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_out, s_in;
static int s_volume = 70;
static void (*s_mic_cb)(bool);

void audio_on_mic(void (*cb)(bool open)) { s_mic_cb = cb; }

static bool i2s_start(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = DMA_BUFS;
    chan.dma_frame_num = FRAME;
    chan.auto_clear_after_cb = true;
    if (i2s_new_channel(&chan, &s_tx, &s_rx) != ESP_OK) return false;
    i2s_std_config_t tx = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = PIN_DOUT, .din = I2S_GPIO_UNUSED },
    };
    tx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx.slot_cfg.ws_width = 32;
    i2s_tdm_config_t rx = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(AUDIO_FS),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = { .mclk = PIN_MCLK, .bclk = PIN_BCLK, .ws = PIN_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_DIN },
    };
    rx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    rx.slot_cfg.total_slot = SLOTS;
    rx.slot_cfg.ws_width = 32;
    rx.slot_cfg.left_align = true;
    return i2s_channel_init_std_mode(s_tx, &tx) == ESP_OK && i2s_channel_init_tdm_mode(s_rx, &rx) == ESP_OK
        && i2s_channel_enable(s_tx) == ESP_OK && i2s_channel_enable(s_rx) == ESP_OK;
}

/* The ES7210's registers after open: 00h is 41h once its reset is released,
   02h is C1h, the 256 x fs clocking. A cold-boot failure on this codec pair
   (main/speaker_app.c) left both at their power-on values. */
static bool es7210_landed(void)
{
    int r00 = audio_mic_reg(0x00), r02 = audio_mic_reg(0x02);
    ESP_LOGI(TAG, "ES7210: reg00 %02X reg02 %02X (want 41 C1)", (unsigned)(r00 & 0xFF), (unsigned)(r02 & 0xFF));
    return r00 == 0x41 && r02 == 0xC1;
}

static bool es7210_open(void)
{
    esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = SLOTS, .channel_mask = 0, .sample_rate = AUDIO_FS, .mclk_multiple = 256 };
    if (esp_codec_dev_open(s_in, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_in_gain(s_in, MIC_GAIN_DB);     /* after open: the open resets the PGAs */
    return true;
}

int audio_mic_reg(int reg)
{
    int v = -1;
    if (!s_in || esp_codec_dev_read_reg(s_in, reg, &v) != ESP_CODEC_DEV_OK) return -1;
    return v & 0xFF;
}

/* RX runs from init and is never stopped, so its DMA queue holds the last
   DMA_BUFS reads of whatever was going on before: drop them. */
static void drain_rx(int16_t *tdm, size_t bytes)
{
    for (int i = 0; i < DMA_BUFS; i++) esp_codec_dev_read(s_in, tdm, bytes);
}

esp_err_t audio_init(void)
{
    if (!i2s_start()) { ESP_LOGE(TAG, "I2S failed"); return ESP_FAIL; }
    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t c8311 = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = lcd_i2c() };
    audio_codec_i2c_cfg_t c7210 = { .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = lcd_i2c() };
    const audio_codec_ctrl_if_t *ctrl8311 = audio_codec_new_i2c_ctrl(&c8311);
    const audio_codec_ctrl_if_t *ctrl7210 = audio_codec_new_i2c_ctrl(&c7210);
    es8311_codec_cfg_t e8311 = {
        .ctrl_if = ctrl8311, .gpio_if = NULL, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC, .pa_pin = -1,
        .master_mode = false, .use_mclk = true, .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    es7210_codec_cfg_t e7210 = {
        .ctrl_if = ctrl7210, .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *if8311 = es8311_codec_new(&e8311);
    const audio_codec_if_t *if7210 = es7210_codec_new(&e7210);
    if (!data_if || !if8311 || !if7210) { ESP_LOGE(TAG, "codecs did not answer"); return ESP_FAIL; }

    esp_codec_dev_cfg_t oc = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = if8311, .data_if = data_if };
    s_out = esp_codec_dev_new(&oc);
    esp_codec_dev_sample_info_t ofs = { .bits_per_sample = 16, .channel = 2, .channel_mask = 0, .sample_rate = AUDIO_FS, .mclk_multiple = 256 };
    if (!s_out || esp_codec_dev_open(s_out, &ofs) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "ES8311 open failed"); return ESP_FAIL; }
    esp_codec_dev_set_out_vol(s_out, s_volume);

    esp_codec_dev_cfg_t ic = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = if7210, .data_if = data_if };
    s_in = esp_codec_dev_new(&ic);
    if (!s_in || !es7210_open()) { ESP_LOGE(TAG, "ES7210 open failed"); return ESP_FAIL; }
    if (!es7210_landed()) {         /* one retry, as on speaker_app.c: close and open again */
        ESP_LOGW(TAG, "ES7210: registers did not land; opening it again");
        esp_codec_dev_close(s_in);
        if (!es7210_open() || !es7210_landed()) ESP_LOGE(TAG, "ES7210: still not right; the mics may be deaf");
    }
    ESP_LOGI(TAG, "speaker and mics up at %d Hz", AUDIO_FS);
    return ESP_OK;
}

int audio_record(int16_t *buf, int max_samples, int max_ms, vad_result_t *why)
{
    static int16_t tdm[FRAME * SLOTS];
    vad_t v;
    vad_init(&v, AUDIO_FS, max_ms);
    int n = 0;
    vad_result_t r = VAD_WAITING;
    if (s_mic_cb) s_mic_cb(true);
    drain_rx(tdm, sizeof tdm);
    while (n + FRAME <= max_samples) {
        if (esp_codec_dev_read(s_in, tdm, sizeof tdm) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "ES7210 read failed: no audio");
            r = VAD_SILENT;
            break;
        }
        for (int i = 0; i < FRAME; i++) buf[n + i] = tdm[i * SLOTS + SLOT_MIC];
        r = vad_feed(&v, buf + n, FRAME);
        n += FRAME;
        if (r == VAD_DONE || r == VAD_SILENT || r == VAD_CAPPED) break;
    }
    if (r == VAD_WAITING || r == VAD_SPEAKING) r = vad_speech_ms(&v) >= 300 ? VAD_CAPPED : VAD_SILENT;   /* buffer full */
    if (s_mic_cb) s_mic_cb(false);
    *why = r;
    return n;
}

bool audio_play_begin(void)
{
    lcd_amp(true);
    return s_out != NULL;
}

bool audio_play_chunk(const int16_t *mono, int samples)
{
    static int16_t st[FRAME * 2];
    while (samples > 0) {
        int k = samples < FRAME ? samples : FRAME;
        for (int i = 0; i < k; i++) st[2 * i] = st[2 * i + 1] = mono[i];
        if (esp_codec_dev_write(s_out, st, k * 2 * (int)sizeof(int16_t)) != ESP_CODEC_DEV_OK) return false;
        mono += k;
        samples -= k;
    }
    return true;
}

void audio_play_end(void)
{
    static int16_t quiet[FRAME * 2];
    esp_codec_dev_write(s_out, quiet, sizeof quiet);   /* let the last of it out of the DMA */
    lcd_amp(false);
}

void audio_set_volume(int level)
{
    s_volume = level < 0 ? 0 : level > 100 ? 100 : level;
    if (s_out) esp_codec_dev_set_out_vol(s_out, s_volume);
}

int audio_volume(void) { return s_volume; }

int audio_slot_levels(int ms, const int16_t *tone, int tone_len, int peak[SLOTS], int rms[SLOTS], int loud[SLOTS])
{
    static int16_t tdm[FRAME * SLOTS];
    double sq[SLOTS] = { 0 };
    for (int c = 0; c < SLOTS; c++) peak[c] = rms[c] = loud[c] = 0;
    if (!s_in) return -1;
    if (s_mic_cb) s_mic_cb(true);
    if (tone) audio_play_begin();
    drain_rx(tdm, sizeof tdm);
    int frames = 0, t = 0;
    for (int k = 0; k < ms / 20; k++) {
        if (tone) {     /* one frame out, one frame in: both queues run on the same clock */
            static int16_t chunk[FRAME];
            for (int i = 0; i < FRAME; i++, t = (t + 1) % tone_len) chunk[i] = tone[t];
            audio_play_chunk(chunk, FRAME);
        }
        if (esp_codec_dev_read(s_in, tdm, sizeof tdm) != ESP_CODEC_DEV_OK) { ESP_LOGE(TAG, "ES7210 read failed"); break; }
        double fsq[SLOTS] = { 0 };
        for (int i = 0; i < FRAME; i++)
            for (int c = 0; c < SLOTS; c++) {
                int v = tdm[i * SLOTS + c];
                if (abs(v) > peak[c]) peak[c] = abs(v);
                fsq[c] += (double)v * v;
            }
        for (int c = 0; c < SLOTS; c++) {
            sq[c] += fsq[c];
            int f = (int)sqrt(fsq[c] / FRAME);      /* this 20 ms, as the end-of-speech detector sees it */
            if (f > loud[c]) loud[c] = f;
        }
        frames += FRAME;
    }
    if (tone) audio_play_end();
    if (s_mic_cb) s_mic_cb(false);
    for (int c = 0; c < SLOTS && frames; c++) rms[c] = (int)sqrt(sq[c] / frames);
    return frames;
}
