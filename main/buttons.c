/* Whatever push buttons the board has, turned into the two intentions. */
#include "buttons.h"

#include "sdkconfig.h"

#if CONFIG_SCREEN_BUTTON_A >= 0

#include "driver/gpio.h"
#include "esp_timer.h"

#include <stdbool.h>

#define PIN_A CONFIG_SCREEN_BUTTON_A
#define PIN_B CONFIG_SCREEN_BUTTON_B
#define HAVE_B (CONFIG_SCREEN_BUTTON_B >= 0)

/* Both buttons pull their pin to ground, so a press reads low. */
#define PRESSED_LEVEL 0

/* Long enough to swallow contact bounce, short enough that deliberate
   repeated presses still register. Also stops a held button repeating. */
#define DEBOUNCE_US (200 * 1000)

/*
 * How long counts as holding it rather than tapping it, on a board with only
 * one button. Long enough not to catch a slow tap, short enough that you are
 * not left wondering whether it registered.
 */
#define HOLD_US (600 * 1000)

static bool s_down[2];
static int64_t s_last_us[2];
static int64_t s_pressed_at;
static bool s_hold_fired;

static const int s_pins[2] = { PIN_A, HAVE_B ? PIN_B : PIN_A };

esp_err_t buttons_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_A) | (HAVE_B ? (1ULL << PIN_B) : 0ULL),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    /* Whatever the buttons read at boot is the resting state, so a board
       powered up with one held down does not report a press immediately. */
    for (int i = 0; i < (HAVE_B ? 2 : 1); i++)
        s_down[i] = gpio_get_level(s_pins[i]) == PRESSED_LEVEL;
    return ESP_OK;
}

#if HAVE_B

button_t buttons_pressed(void)
{
    /* Two buttons, so each is a direction and there is nothing to interpret. */
    static const button_t which[2] = { BUTTON_PREV, BUTTON_NEXT };
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < 2; i++) {
        bool down = gpio_get_level(s_pins[i]) == PRESSED_LEVEL;
        bool was = s_down[i];
        s_down[i] = down;
        /* Only the press edge counts; the release is not an intention. */
        if (down && !was && now - s_last_us[i] > DEBOUNCE_US) {
            s_last_us[i] = now;
            return which[i];
        }
    }
    return BUTTON_NONE;
}

#else

/*
 * One button, so it has to carry both directions: a tap goes forward and
 * holding it goes back.
 *
 * Going back fires the moment the hold is long enough rather than on release,
 * so the board answers while your finger is still down and you are not left
 * guessing whether you held it long enough. Going forward can only be known
 * on release, because until then it might still become a hold.
 */
button_t buttons_pressed(void)
{
    int64_t now = esp_timer_get_time();
    bool down = gpio_get_level(s_pins[0]) == PRESSED_LEVEL;
    bool was = s_down[0];
    s_down[0] = down;

    if (down && !was) {
        if (now - s_last_us[0] <= DEBOUNCE_US) return BUTTON_NONE;
        s_last_us[0] = now;
        s_pressed_at = now;
        s_hold_fired = false;
        return BUTTON_NONE;
    }
    if (down && was && !s_hold_fired && now - s_pressed_at > HOLD_US) {
        s_hold_fired = true;
        return BUTTON_PREV;
    }
    if (!down && was) {
        if (s_hold_fired) return BUTTON_NONE;   /* already answered as a hold */
        return BUTTON_NEXT;
    }
    return BUTTON_NONE;
}

#endif /* HAVE_B */

#else  /* no buttons on this board */

esp_err_t buttons_init(void) { return ESP_ERR_NOT_SUPPORTED; }
button_t buttons_pressed(void) { return BUTTON_NONE; }

#endif
