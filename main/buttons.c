/* The two push buttons on the LilyGO T-Display-S3. */
#include "buttons.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include <stdbool.h>

#define PIN_BUTTON_PREV  0      /* BOOT; has an external pull-up */
#define PIN_BUTTON_NEXT 14

/* Both buttons pull their pin to ground, so a press reads low. */
#define PRESSED_LEVEL 0

/* Long enough to swallow contact bounce, short enough that deliberate
   repeated presses still register. Also stops a held button repeating. */
#define DEBOUNCE_US (200 * 1000)

static bool s_down[2];
static int64_t s_last_us[2];

static const int s_pins[2] = { PIN_BUTTON_PREV, PIN_BUTTON_NEXT };
static const button_t s_which[2] = { BUTTON_PREV, BUTTON_NEXT };

esp_err_t buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_PREV) | (1ULL << PIN_BUTTON_NEXT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    /* Whatever the buttons read at boot is the resting state, so a board
       powered up with one held down does not report a press immediately. */
    for (int i = 0; i < 2; i++) s_down[i] = gpio_get_level(s_pins[i]) == PRESSED_LEVEL;
    return ESP_OK;
}

button_t buttons_pressed(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 2; i++) {
        bool down = gpio_get_level(s_pins[i]) == PRESSED_LEVEL;
        bool was = s_down[i];
        s_down[i] = down;
        /* Only the press edge counts; the release is not an intention. */
        if (down && !was && now - s_last_us[i] > DEBOUNCE_US) {
            s_last_us[i] = now;
            return s_which[i];
        }
    }
    return BUTTON_NONE;
}
