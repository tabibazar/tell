#include "pdm.h"

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_log.h"

static const char *TAG = "pdm";
static i2s_chan_handle_t s_tx;
static int s_gpio = -1, s_volume = 60;
static bool s_on;

esp_err_t pdm_init(int gpio)
{
    s_gpio = gpio;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear = true;                          /* an underrun plays silence, not the last buffer again */
    esp_err_t e = i2s_new_channel(&chan, &s_tx, NULL);
    if (e != ESP_OK) return e;
    i2s_pdm_tx_config_t cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(PDM_FS),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .clk = I2S_GPIO_UNUSED, .dout = gpio },
    };
    e = i2s_channel_init_pdm_tx_mode(s_tx, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "PDM init: %s", esp_err_to_name(e)); return e; }
    gpio_set_level(gpio, 0);
    ESP_LOGI(TAG, "PDM audio on GPIO%d", gpio);
    return ESP_OK;
}

void pdm_begin(void)
{
    if (s_on || !s_tx) return;
    i2s_channel_enable(s_tx);
    s_on = true;
}

bool pdm_write(const int16_t *mono, int samples)
{
    static int16_t buf[256];
    if (!s_on) return false;
    int g = s_volume * s_volume;                     /* perceptual: volume squared, 0..10000 */
    while (samples > 0) {
        int k = samples < 256 ? samples : 256;
        for (int i = 0; i < k; i++) buf[i] = (int16_t)((int32_t)mono[i] * g / 10000);
        size_t done;
        if (i2s_channel_write(s_tx, buf, k * sizeof(int16_t), &done, 1000) != ESP_OK) return false;
        mono += k;
        samples -= k;
    }
    return true;
}

void pdm_end(void)
{
    if (!s_on) return;
    static const int16_t quiet[PDM_FS / 20];         /* 50 ms, so the last of it leaves the DMA */
    size_t done;
    i2s_channel_write(s_tx, quiet, sizeof quiet, &done, 1000);
    i2s_channel_disable(s_tx);
    gpio_set_level(s_gpio, 0);
    s_on = false;
}

void pdm_volume(int level) { s_volume = level < 0 ? 0 : level > 100 ? 100 : level; }
int pdm_get_volume(void) { return s_volume; }
