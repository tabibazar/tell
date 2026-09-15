#include "tempsense.h"

#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "temp";

/*
 * Not every ESP32 has one. The S3 does; the original ESP32 does not -- it is
 * a peripheral that arrived with the later parts, and its driver header will
 * not even compile against the older target.
 *
 * So the whole driver stands down rather than the callers being littered with
 * board tests. Everything downstream already copes with a sensor that will
 * not answer, because a board can also simply fail to bring one up.
 */
#if SOC_TEMP_SENSOR_SUPPORTED

#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t s_sensor;

esp_err_t tempsense_init(void)
{
    if (s_sensor != NULL) return ESP_OK;

    /*
     * The range picks the sensor's internal offset, and it has to fall inside
     * one of the hardware's own ranges rather than spanning several: -10 to
     * 100 is rejected outright with "Cannot select the correct range". 20 to
     * 100 is one of them, and a die that is being asked its temperature is
     * running, so it is always well above twenty.
     */
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    esp_err_t err = temperature_sensor_install(&cfg, &s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no temperature sensor: %s", esp_err_to_name(err));
        s_sensor = NULL;
        return err;
    }
    err = temperature_sensor_enable(s_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_sensor);
        s_sensor = NULL;
        return err;
    }

    float c = 0.0f;
    if (tempsense_read(&c)) ESP_LOGI(TAG, "die at %.1f C", (double)c);
    return ESP_OK;
}

bool tempsense_read(float *celsius)
{
    if (s_sensor == NULL) return false;
    return temperature_sensor_get_celsius(s_sensor, celsius) == ESP_OK;
}

uint32_t tempsense_entropy(void)
{
    if (s_sensor == NULL) return 0;

    /*
     * Consecutive readings of a still die disagree in their last bits, which
     * is the noise this wants. Each reading contributes one bit, taken from
     * the hundredths of a degree and folded in with a shift and an xor so a
     * stuck sensor gives a constant rather than a plausible-looking number.
     */
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        float c = 0.0f;
        if (!tempsense_read(&c)) break;
        uint32_t hundredths = (uint32_t)(c * 100.0f);
        bits = (bits << 1) ^ (hundredths & 1u);
    }
    return bits;
}

#else  /* no sensor on this silicon */

esp_err_t tempsense_init(void)
{
    ESP_LOGI(TAG, "this chip has no temperature sensor; doing without");
    return ESP_ERR_NOT_SUPPORTED;
}

bool tempsense_read(float *celsius) { (void)celsius; return false; }

/* Zero rather than a guess: the caller mixes this with esp_random(), and a
   fabricated value would be worse than none. */
uint32_t tempsense_entropy(void) { return 0; }

#endif
