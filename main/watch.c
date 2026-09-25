/*
 * watch's power, button and battery, from Waveshare's ESP32-S3-LCD-1.69 V2
 * schematic -- the unit is the non-touch board; the Touch V2.1 is wired the
 * same here -- and the first revision's, to tell the two apart safely.
 *
 * The power latch. The battery reaches the board through a P-FET that is off
 * until something pulls its gate down: the function button while it is held,
 * or SYS_EN (GPIO41) through a transistor. So on battery the board only stays
 * up after the finger lets go if the firmware has driven SYS_EN high by then.
 * On USB the latch is irrelevant: the charger's rail feeds the regulator
 * directly and nothing we do can switch the board off.
 *
 * The two revisions moved the latch. The old one has SYS_EN on GPIO35 and the
 * RTC's open-drain INT on GPIO41 -- driving 41 high there fights the RTC, and
 * 35 is an octal-PSRAM data line on this chip that we must never touch. So the
 * revision is established from inputs alone before any output is driven:
 * GPIO42 (V2.1's buzzer base, held near 0.3 V by R14/R15 against our weak
 * pull-up) reads LOW on V2.1 and floats HIGH on the old board. GPIO40
 * (SYS_OUT, R20 to 3V3 on V2.1) confirms, except while the button is held,
 * when it reads LOW on both -- which is exactly a battery cold boot, so 42
 * decides and 40 only corroborates.
 */
#include "watch.h"

#include "sdkconfig.h"

#if CONFIG_SCREEN_BOARD_TOUCH_LCD_169

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SYS_OUT  40      /* the function button; LOW while held */
#define PIN_SYS_EN   41      /* HIGH holds the battery on */
#define PIN_BUZZ     42      /* the buzzer transistor's base; LOW when idle */
#define BAT_CHANNEL  ADC_CHANNEL_0   /* GPIO1: B+ through 200k over 100k */

#define LONG_PRESS_US   (2000 * 1000)
#define DEBOUNCE_US     (40 * 1000)

static const char *TAG = "watch";
static watch_rev_t s_rev;

watch_rev_t watch_power_init(void)
{
    /* Inputs only, with the internal pulls the reading depends on. Never a
       pull-up on 41: against R22/R24 it half-switches the latch transistor. */
    gpio_config_t in42 = { .pin_bit_mask = 1ULL << PIN_BUZZ, .mode = GPIO_MODE_INPUT,
                           .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&in42);
    gpio_config_t in40 = { .pin_bit_mask = 1ULL << PIN_SYS_OUT, .mode = GPIO_MODE_INPUT,
                           .pull_down_en = GPIO_PULLDOWN_ENABLE };
    gpio_config(&in40);
    esp_rom_delay_us(300);
    int l42 = gpio_get_level(PIN_BUZZ);
    int l40 = gpio_get_level(PIN_SYS_OUT);

    /* SYS_OUT has its own 10k pull-up on V2.1; ours comes off again. */
    gpio_config_t key = { .pin_bit_mask = 1ULL << PIN_SYS_OUT, .mode = GPIO_MODE_INPUT };
    gpio_config(&key);

    if (l42 == 0) {
        s_rev = WATCH_REV_V21;
        gpio_config_t out = { .pin_bit_mask = (1ULL << PIN_SYS_EN) | (1ULL << PIN_BUZZ),
                              .mode = GPIO_MODE_OUTPUT };
        gpio_config(&out);
        gpio_set_level(PIN_SYS_EN, 1);
        gpio_set_level(PIN_BUZZ, 0);
        /* Held through a software reset or a panic too, so a crash on
           battery restarts rather than switching the watch off. NOT through
           deep sleep: on the S3 that also needs gpio_deep_sleep_hold_en(),
           or SYS_EN drops the moment the chip sleeps and the watch dies. */
        gpio_hold_en(PIN_SYS_EN);
        gpio_hold_en(PIN_BUZZ);
        ESP_LOGI(TAG, "V2.1 (GPIO42 low, GPIO40 %s): power latched", l40 ? "high" : "low, key held");
    } else if (l40 == 0) {
        s_rev = WATCH_REV_OLD;
        ESP_LOGW(TAG, "old revision (GPIO42 high, GPIO40 low): SYS_EN is on a PSRAM "
                      "line here, so the latch is left alone -- runs on USB only");
    } else {
        s_rev = WATCH_REV_UNKNOWN;
        ESP_LOGW(TAG, "revision unclear (GPIO42 high, GPIO40 high): latch left alone");
    }
    return s_rev;
}

watch_key_t watch_key_poll(int64_t now_us)
{
    static bool down, reported_long;
    static int64_t since_us, edge_us;
    bool pressed = gpio_get_level(PIN_SYS_OUT) == 0;
    if (pressed != down && now_us - edge_us >= DEBOUNCE_US) {
        edge_us = now_us;
        down = pressed;
        if (down) {
            since_us = now_us;
            reported_long = false;
        } else if (!reported_long) {
            return WATCH_KEY_SHORT;
        }
    }
    if (down && !reported_long && now_us - since_us >= LONG_PRESS_US) {
        reported_long = true;
        return WATCH_KEY_LONG;
    }
    return WATCH_KEY_NONE;
}

bool watch_power_off(void)
{
    if (s_rev != WATCH_REV_V21) return false;
    /* While the button is held its own diode keeps the battery on, so the
       latch only matters once it is released: wait for that first. */
    for (int i = 0; i < 300 && gpio_get_level(PIN_SYS_OUT) == 0; i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "releasing the power latch");
    gpio_hold_dis(PIN_SYS_EN);
    gpio_set_level(PIN_SYS_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(300));
    /* Still here: USB is feeding the regulator. Take the latch back so a
       later unplug does not drop the board. */
    gpio_set_level(PIN_SYS_EN, 1);
    gpio_hold_en(PIN_SYS_EN);
    ESP_LOGI(TAG, "still powered (USB): darkening instead");
    return false;
}

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_adc_ok, s_adc_tried;

static void adc_start(void)
{
    s_adc_tried = true;
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) return;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, BAT_CHANNEL, &ch) != ESP_OK) return;
    adc_cali_curve_fitting_config_t cal = { .unit_id = ADC_UNIT_1, .chan = BAT_CHANNEL,
                                            .atten = ADC_ATTEN_DB_12,
                                            .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
    s_adc_ok = true;
}

int watch_battery_mv(void)
{
    if (!s_adc_tried) adc_start();
    if (!s_adc_ok) return -1;
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, BAT_CHANNEL, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;
        n++;
    }
    if (n == 0) return -1;
    return sum / n * 3;      /* the 200k/100k divider */
}

int watch_battery_pct(void)
{
    /*
     * A LiPo's resting curve, coarsely: 4.2 V full, 3.3 V as good as empty.
     * Read under the board's own load and while charging it runs high, so
     * this is a gauge, not a fuel figure -- which is what a power-reserve
     * hand on a watch has always been. Smoothed so the hand does not twitch.
     */
    static int smooth = -1;
    int mv = watch_battery_mv();
    if (mv < 0) return -1;
    if (mv < 2500) return -1;                /* no cell: nothing worth showing */
    int pct = (mv - 3300) * 100 / (4200 - 3300);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    smooth = smooth < 0 ? pct : (smooth * 7 + pct) / 8;
    return smooth;
}

#endif /* CONFIG_SCREEN_BOARD_TOUCH_LCD_169 */
