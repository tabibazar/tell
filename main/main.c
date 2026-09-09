#include "display.h"
#include "ble_uart.h"
#include "gt911.h"
#include "level.h"
#include "qmi8658.h"
#include "pagedefs.h"
#include "pages.h"
#include "view_common.h"
#include "ds3231.h"
#include "settings.h"
#include "timecalc.h"
#include "usagedata.h"

#include "palette.h"
#include "views.h"

#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch poll interval */


static uint32_t s_base_secs;
static int64_t  s_base_us;
static bool     s_synced;

/* The battery-backed clock, when one is on the bus. A Mac's sync is written
   to it from the main loop rather than from the BLE task that receives it,
   so the I2C bus is only ever driven from one task. */
static bool     s_rtc;
static bool     s_rtc_pending;      /* a sync arrived; copy it to the chip */
static int64_t  s_rtc_checked_us;
#define RTC_RECHECK_US (3600 * 1000000LL)

static usagedata_t s_data;
static settings_t s_settings;
#define MESSAGE_MAX 512
static char s_message[MESSAGE_MAX + 1];
static pages_t s_pages;

/* Forces a redraw when the page or the displayed second changes. */
static page_t s_drawn_page = PAGE_COUNT;
static int s_drawn_second = -1;

/* When the current page was last drawn, for pages that refresh on their own. */
static int64_t s_page_drawn_us = 0;

/* The title bars carry the time, so every page redraws when the minute
   changes -- quietly, without replaying its grow-in animation. */
static int s_drawn_minute = -1;
static bool s_quiet_redraw = false;
static bool s_zone_dirty = false;     /* a Mac sent a zone; keep it in flash */

/* Auto-jump: while Claude is busy the live page shows itself, and when the
   work stops the clock comes back. Only from the clock or the saver, so a
   page someone is reading is never taken away, and only until they tap. */
static bool s_busy = false;
static bool s_auto_jumped = false;
static int64_t s_busy_check_us = 0;
#define BUSY_CHECK_US (10 * 1000000LL)

static bool claude_busy(const ud_view_t *v, int64_t now)
{
    if (!v->now.present || !v->now.have_last) return false;
    int64_t since = v->now.last_secs + (now - v->now.last_sent_us) / 1000000;
    return since < UD_BUSY_SECS;
}

/* The spirit level, on the Feather only, driven by its QMI8658. The big board
   has no IMU and is not compiled with any of this. */
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
#define HAVE_LEVEL 1
static bool s_imu;

/*
 * Which sensor axis is which on the panel. An accelerometer at rest reads
 * +1 g along the axis pointing UP, so gravity is minus the reading: G = -a.
 *
 * Measured on the board rather than assumed. Standing on its long edge the
 * sensor reads (+0.99, +0.15, -0.20), so +X is up and the panel's downward
 * component is +ax. Laid flat it read (+0.50, +0.12, -0.96): X and Z traded
 * places while Y barely moved, so Y is the board's long edge -- the panel's
 * horizontal -- and Z is the screen's normal.
 *
 * The signs are another matter. Which way round they go depends on how the
 * breakout is soldered and on which way the board is facing when you look at
 * it, and no still reading can tell you: gravity has no component along an
 * axis that is level. They have been wrong twice, so they are no longer
 * compiled in. Send "!flip x", "!flip y" or "!flip swap" and the board
 * changes them and remembers, which takes a second instead of a reflash.
 */
static int8_t s_axis_sx = -1;    /* panel +x, to the right, from sensor y */
static int8_t s_axis_sy =  1;    /* panel +y, downwards,     from sensor x */
static bool   s_axis_swap;       /* the two sensor axes the other way round */

static void axis_load(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READONLY, &h) != ESP_OK) return;
    int8_t v;
    uint8_t b;
    if (nvs_get_i8(h, "axsx", &v) == ESP_OK) s_axis_sx = v;
    if (nvs_get_i8(h, "axsy", &v) == ESP_OK) s_axis_sy = v;
    if (nvs_get_u8(h, "axswap", &b) == ESP_OK) s_axis_swap = b != 0;
    nvs_close(h);
    ESP_LOGI(TAG, "axis %+d %+d%s", s_axis_sx, s_axis_sy,
             s_axis_swap ? " swapped" : "");
}

static void axis_save(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i8(h, "axsx", s_axis_sx);
    nvs_set_i8(h, "axsy", s_axis_sy);
    nvs_set_u8(h, "axswap", s_axis_swap ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* "x", "y", "swap" or "reset", anywhere in the payload. */
static void axis_command(const char *text)
{
    if (strstr(text, "swap")) s_axis_swap = !s_axis_swap;
    else if (strstr(text, "reset")) { s_axis_sx = -1; s_axis_sy = 1; s_axis_swap = false; }
    else if (strchr(text, 'x')) s_axis_sx = (int8_t)-s_axis_sx;
    else if (strchr(text, 'y')) s_axis_sy = (int8_t)-s_axis_sy;
    axis_save();
    ESP_LOGI(TAG, "axis now %+d %+d%s", s_axis_sx, s_axis_sy,
             s_axis_swap ? " swapped" : "");
}

const char *axis_describe(void)
{
    static char buf[24];
    snprintf(buf, sizeof buf, "axis %c%c%s",
             s_axis_sx < 0 ? '-' : '+', s_axis_sy < 0 ? '-' : '+',
             s_axis_swap ? " sw" : "");
    return buf;
}

static void gravity_from(const qmi8658_sample_t *s, float *gx, float *gy)
{
    float across = s->ay, along = s->ax;
    if (s_axis_swap) { float t = across; across = along; along = t; }
    *gx = (float)s_axis_sx * across;
    *gy = (float)s_axis_sy * along;
}

/* The level keeps its own filtered copy of gravity, in g and much slower than
   the liquid's. The liquid wants to feel the board move; an instrument wants
   to be read, and at the animation filter's speed the tenths digit never
   stops moving. This corner is about a fifth of a hertz -- it takes a second
   to catch up with a deliberate tilt and ignores everything faster. */
#define LEVEL_ALPHA 0.06f
static float s_lx, s_ly, s_lz;

/* The surface the board is standing on, as captured by "!zero": angles are
   reported relative to it. A desk is not a reference plane and neither is a
   sensor soldered by hand, so "level" is most usefully "level with whatever
   this is sitting on now". "!zero reset" goes back to absolute. */
static float s_zero_x, s_zero_y;
static bool  s_zeroed;

/* Holding a board flat by hand is harder than it sounds, so the level keeps a
   clock: how long it has been true for, and the longest it ever has. The best
   is written to flash only when a run ends and only if it beat the old one --
   every frame would be thousands of writes a minute. */
static float s_hold_s, s_best_hold_s;
static int64_t s_level_last_us;

static void best_save(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "hold", &s_best_hold_s, sizeof s_best_hold_s);
    nvs_commit(h);
    nvs_close(h);
}

static void zero_load(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READONLY, &h) != ESP_OK) return;
    size_t hn = sizeof(float);
    nvs_get_blob(h, "hold", &s_best_hold_s, &hn);
    size_t n = sizeof(float);
    if (nvs_get_blob(h, "zerox", &s_zero_x, &n) == ESP_OK) {
        n = sizeof(float);
        if (nvs_get_blob(h, "zeroy", &s_zero_y, &n) == ESP_OK) s_zeroed = true;
    }
    nvs_close(h);
    if (s_zeroed)
        ESP_LOGI(TAG, "level zeroed at %+.3f %+.3f",
                 (double)s_zero_x, (double)s_zero_y);
}

static void zero_save(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READWRITE, &h) != ESP_OK) return;
    if (s_zeroed) {
        nvs_set_blob(h, "zerox", &s_zero_x, sizeof s_zero_x);
        nvs_set_blob(h, "zeroy", &s_zero_y, sizeof s_zero_y);
    } else {
        nvs_erase_key(h, "zerox");
        nvs_erase_key(h, "zeroy");
    }
    nvs_commit(h);
    nvs_close(h);
}

/* "!zero" takes whatever the board is resting on as true; "!zero reset" goes
   back to absolute. The reading used is the level's own heavily filtered one,
   so a zero is a settled measurement rather than one frame of noise. */
static void zero_command(const char *text)
{
    if (strstr(text, "reset")) {
        s_zeroed = false;
    } else {
        float ax = s_lx < -1.0f ? -1.0f : (s_lx > 1.0f ? 1.0f : s_lx);
        float ay = s_ly < -1.0f ? -1.0f : (s_ly > 1.0f ? 1.0f : s_ly);
        s_zero_x = asinf(ax) * 180.0f / (float)M_PI;
        s_zero_y = asinf(ay) * 180.0f / (float)M_PI;
        s_zeroed = true;
        ESP_LOGI(TAG, "level zeroed at %+.2f %+.2f",
                 (double)s_zero_x, (double)s_zero_y);
    }
    zero_save();
}

static void draw_level(canvas_t *c)
{
    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) != ESP_OK) return;

    float gx, gy;
    gravity_from(&sample, &gx, &gy);
    float gz = -sample.az;              /* out of the screen, toward you */

    s_lx += (gx - s_lx) * LEVEL_ALPHA;
    s_ly += (gy - s_ly) * LEVEL_ALPHA;
    s_lz += (gz - s_lz) * LEVEL_ALPHA;

    /* How far each of the panel's own axes is off horizontal. With the board
       flat this is the natural reading and both are zero on a true surface.
       asin, not atan2: the in-plane component of gravity IS the sine of the
       tilt, and it needs no assumption about which way up the board is. */
    float ax = s_lx < -1.0f ? -1.0f : (s_lx > 1.0f ? 1.0f : s_lx);
    float ay = s_ly < -1.0f ? -1.0f : (s_ly > 1.0f ? 1.0f : s_ly);
    float tx = asinf(ax) * 180.0f / (float)M_PI;
    float ty = asinf(ay) * 180.0f / (float)M_PI;
    if (s_zeroed) { tx -= s_zero_x; ty -= s_zero_y; }

    /* The clock. It runs while the board is true and resets the moment it is
       not, which is the whole game. */
    int64_t now = esp_timer_get_time();
    float dt = s_level_last_us
             ? (float)(now - s_level_last_us) / 1000000.0f : 0.0f;
    s_level_last_us = now;
    if (dt > 0.5f) dt = 0.0f;        /* arriving on the page is not a run */

    if (level_is_true(tx, ty)) {
        s_hold_s += dt;
        if (s_hold_s > s_best_hold_s) s_best_hold_s = s_hold_s;
    } else if (s_hold_s > 0.0f) {
        /* A run just ended: this is the one moment worth a flash write. */
        if (s_hold_s >= s_best_hold_s) best_save();
        s_hold_s = 0.0f;
    }

    /* One letter, bottom right: z means the angles are relative to a surface
       taken as true with "!zero", nothing means they are absolute. */
    level_draw(c, tx, ty, s_hold_s, s_best_hold_s, s_zeroed ? "z" : "");
    display_blit();
}

#else
#define HAVE_LEVEL 0
#endif

/* Charts grow into place when a page appears; 0 means no animation running. */
#define ANIM_US (600 * 1000LL)
static int64_t s_anim_start = 0;

/* Screensaver. In its default mode it cycles the pages, each for a set
   number of seconds; in the other the clock drifts to a new spot each minute
   so no pixel stays lit. Position is derived from the minute, so it is
   stable within one. */
static bool s_saver = false;
static int s_saver_minute = -1;
static int s_saver_x, s_saver_y;
static int64_t s_cycle_last = 0;

static void apply_settings(void)
{
    pages_set_saver(&s_pages, (int64_t)s_settings.saver_min * 60 * 1000000LL);
}

/* Where the board rests: the clock, or the live page if that was chosen and
   this board has it. Boot, an expired message and the end of a busy spell
   all land here. */
static page_t home_page(void)
{
    if (s_settings.home_now && (s_pages.available & PAGE_BIT(PAGE_NOW))) return PAGE_NOW;
    return PAGE_CLOCK;
}

/* The pages the cycling saver leaves out: settings, because a slideshow
   should not land on a control panel, and the message page when nothing
   has been sent, because "nothing sent yet" is not worth twenty seconds. */
static unsigned cycle_skip(void)
{
    unsigned skip = 0;
    for (int i = 0; i < PAGE_COUNT; i++)
        if (!page_defs[i].in_saver) skip |= PAGE_BIT(i);
    if (s_message[0] == '\0') skip |= PAGE_BIT(PAGE_MESSAGE);
    return skip;
}

static void on_time(uint32_t secs)
{
    s_base_secs = secs;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    s_drawn_second = -1;
    s_rtc_pending = true;
}

/* The board's idea of the time right now. */
static uint32_t now_secs(int64_t now)
{
    return timecalc_advance(s_base_secs, (uint64_t)(now - s_base_us));
}

static void on_message(const char *text, size_t len)
{
    int64_t now = esp_timer_get_time();

    ud_kind_t kind = usagedata_parse(&s_data, text, now);
    if (kind == UD_CLOCK) {
        /* Arrives on a timer like the charts, so it must not steal the view. */
        if (s_pages.current == PAGE_CLOCK) s_drawn_second = -1;
        else s_drawn_minute = -1;            /* redraw the title strip quietly */
        if (s_data.have_utc) s_zone_dirty = true;
        return;
    }
#if HAVE_LEVEL
    /* Commands, not data: unlike the charts these were asked for, so they do
       take the view. */
    if (kind == UD_LEVEL || kind == UD_FLIP || kind == UD_ZERO) {
        if (kind == UD_FLIP) axis_command(text);
        if (kind == UD_ZERO) zero_command(text);
        pages_show(&s_pages, PAGE_LEVEL, now);
        s_drawn_page = PAGE_COUNT;
        return;
    }
#endif
    if (kind != UD_NONE) {
        /* Data arrives on a timer, so it must never steal the view: refresh
           the numbers, and redraw only if a page it feeds is showing. */
        if (page_defs[s_pages.current].feeds & UD_FEED(kind)) s_drawn_page = PAGE_COUNT;
        if (kind == UD_NOW) s_busy_check_us = 0;      /* re-judge busy at once */
        return;
    }
    if (len == 0) {
        s_message[0] = '\0';
        pages_show(&s_pages, home_page(), now);
    } else {
        size_t n = len < MESSAGE_MAX ? len : MESSAGE_MAX;
        memcpy(s_message, text, n);
        s_message[n] = '\0';
        pages_show(&s_pages, PAGE_MESSAGE, now);
    }
    s_drawn_page = PAGE_COUNT;    /* force a redraw */
}

/* Whatever was last sent with tell. Wrapped, not a scrolling log: this page
   is for messages you choose to send, so the whole message should be here. */
static void draw_message(canvas_t *c)
{
    if (s_message[0] == '\0') {
        canvas_clear(c);
        canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
        canvas_puts(c, 1, 0, "MESSAGE", PAL_FG);
        canvas_puts(c, 1, 2, "nothing sent yet", PAL_DIM);
        canvas_puts(c, 1, 3, "  tell --device big \"hello\"", PAL_DIM);
        return;
    }
    canvas_text(c, s_message);
    vw_menu_tab(c);
}

/* Date above the digits, weather below, both sent from the Mac. Centred so
   they read as part of the clock rather than as a caption. */
static void draw_clock_extras(canvas_t *c)
{
    if (s_data.date[0]) {
        int col = (c->cols - (int)strlen(s_data.date)) / 2;
        canvas_puts(c, col < 0 ? 0 : col, 1, s_data.date, PAL_FG);
    }
    if (s_data.weather[0]) {
        int col = (c->cols - (int)strlen(s_data.weather)) / 2;
        canvas_puts(c, col < 0 ? 0 : col, c->rows - 2, s_data.weather, PAL_A0);
    }
}

/* Draws the clock somewhere new each minute. Deliberately not random per
   frame: it must hold still while you read it. */
static void draw_saver(canvas_t *c, uint32_t secs)
{
    char buf[9];
    timecalc_format_hms(secs, buf);

    int tw, th;
    canvas_big_size(c, buf, &tw, &th);

    int minute = (int)(secs / 60);
    if (minute != s_saver_minute) {
        s_saver_minute = minute;
        unsigned seed = (unsigned)minute * 1103515245u + 12345u;
        int spare_x = c->w - tw;
        int spare_y = c->h - th;
        s_saver_x = spare_x > 0 ? (int)((seed >> 16) % (unsigned)(spare_x + 1)) : 0;
        seed = seed * 1103515245u + 12345u;
        s_saver_y = spare_y > 0 ? (int)((seed >> 16) % (unsigned)(spare_y + 1)) : 0;
    }

    canvas_big_at(c, buf, s_saver_x, s_saver_y);
    display_blit();
}

static void draw_clock(canvas_t *c, int64_t now)
{
    if (!s_synced) {
        if (s_drawn_second != -2) {
            s_drawn_second = -2;
            canvas_big(c, "--:--:--");
            draw_clock_extras(c);
            vw_menu_tab(c);
            display_blit();
        }
        return;
    }
    uint32_t secs = timecalc_advance(s_base_secs, (uint64_t)(now - s_base_us));
    if ((int)secs == s_drawn_second) return;
    char buf[9];
    timecalc_format_hms(secs, buf);
    canvas_big(c, buf);
    draw_clock_extras(c);
    vw_menu_tab(c);
    display_blit();
    s_drawn_second = (int)secs;
}

void app_main(void)
{
    if (display_init() != ESP_OK) {
        /* A panel that failed to start cannot report its own failure. */
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

    bool touch = false, big = false;
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    big = true;
    touch = gt911_init() == ESP_OK;
#endif
    /* Which pages this board offers: the data pages need the big panel, and
       the menu and settings need a finger. */
    unsigned available = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        const page_def_t *pd = &page_defs[i];
        if (!pd->everywhere && !big) continue;
        if (pd->needs_touch && !touch) continue;
        available |= PAGE_BIT(i);
    }
#if HAVE_LEVEL
    /* The level needs a sensor, which the page table cannot know about: it
       describes boards, and this is a question about one board's hardware. */
    s_imu = qmi8658_init() == ESP_OK;
    if (!s_imu) available &= ~PAGE_BIT(PAGE_LEVEL);
#else
    available &= ~PAGE_BIT(PAGE_LEVEL);
#endif
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    /* The clock chip shares the touch bus. If it knows the time, start from
       it, so the display is right before any Mac has said anything. */
    s_rtc = ds3231_init() == ESP_OK;
    uint32_t rtc_secs;
    if (s_rtc && ds3231_read(&rtc_secs)) {
        on_time(rtc_secs);
        s_rtc_pending = false;          /* it came from the chip; no need to write it back */
        ESP_LOGI(TAG, "clock set from the RTC");
    }
#endif
    pages_init(&s_pages, available);
    settings_defaults(&s_settings);

    canvas_t *c = display_canvas();

    if (ble_uart_start(on_message, on_time) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        canvas_text(c, "BLE FAILED");
        display_blit();
        return;
    }
    /* After BLE, which is where NVS gets initialised. */
    settings_load(&s_settings);
    apply_settings();
    /* The zone the Mac last reported, so UTC shows from the RTC's time before
       any Mac has spoken this boot. */
    if (settings_load_zone(&s_data.utc_offset_min, s_data.tz, (int)sizeof s_data.tz))
        s_data.have_utc = true;
#if HAVE_LEVEL
    if (s_imu) { axis_load(); zero_load(); }
#endif
    pages_show(&s_pages, home_page(), esp_timer_get_time());

    int64_t last_beat = 0;
    int64_t last_wake = esp_timer_get_time();
    /* The merged view is a few kilobytes; the main task's stack is not. */
    static ud_view_t v;

    for (;;) {
        /* Sleep for what is left of the frame, not a whole frame on top of
           the work. vTaskDelay is time added after everything else has run,
           so a flat 33 ms after 13 ms of solving and blitting gives 23 fps,
           not 30. The tick is 10 ms, so this lands on the tick below. */
        int64_t period_us = (int64_t)TICK_MS * 1000;
#if HAVE_LEVEL
        if (s_imu && s_pages.current == PAGE_LEVEL)
            period_us = 33000;      /* 30 fps while the bubble is moving */
#endif
        int64_t rest_ms = (period_us - (esp_timer_get_time() - last_wake)) / 1000;
        vTaskDelay(rest_ms > 1 ? pdMS_TO_TICKS(rest_ms) : 1);

        int64_t now = esp_timer_get_time();
        last_wake = now;

        if (s_zone_dirty) {
            /* Written from here rather than the BLE task, like the RTC. */
            s_zone_dirty = false;
            static int saved_off = INT_MIN;
            static char saved_tz[8];
            if (s_data.utc_offset_min != saved_off || strcmp(s_data.tz, saved_tz) != 0) {
                if (settings_save_zone(s_data.utc_offset_min, s_data.tz)) {
                    saved_off = s_data.utc_offset_min;
                    strncpy(saved_tz, s_data.tz, sizeof saved_tz - 1);
                    ESP_LOGI(TAG, "zone %s (%d min) kept", s_data.tz, s_data.utc_offset_min);
                }
            }
        }
        if (s_rtc && s_rtc_pending && s_synced) {
            /* A Mac just synced us; its clock is NTP-disciplined, so the chip
               takes that time and holds it through the next power cycle. */
            s_rtc_pending = false;
            s_rtc_checked_us = now;
            if (ds3231_write(now_secs(now))) ESP_LOGI(TAG, "RTC set from the Mac");
        }
        if (s_rtc && now - s_rtc_checked_us > RTC_RECHECK_US) {
            /* The ESP timer drifts seconds a day; the chip does not. Once an
               hour without a Mac's sync, take the chip's word for it. */
            s_rtc_checked_us = now;
            uint32_t secs;
            if (ds3231_read(&secs)) {
                on_time(secs);
                s_rtc_pending = false;
                ESP_LOGI(TAG, "clock re-read from the RTC");
            }
        }

        if (touch && gt911_tapped()) {
            int tx, ty, row, choice;
            page_t target;
            gt911_point(&tx, &ty);
            if (s_saver) {
                /* The first tap dismisses the saver rather than also changing
                   the page, which would be a surprise. */
                s_pages.last_activity_us = now;
                ESP_LOGI(TAG, "tap at %d,%d -> wake", tx, ty);
            } else if (s_pages.current != PAGE_MENU && vw_menu_tab_hit(c, tx, ty)) {
                pages_show(&s_pages, PAGE_MENU, now);
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "tap at %d,%d -> menu tab", tx, ty);
            } else if (s_pages.current == PAGE_MENU
                       && views_menu_hit(c, &s_pages, tx, ty, &target)) {
                pages_show(&s_pages, target, now);
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "tap at %d,%d -> menu -> page %d", tx, ty, (int)target);
            } else if (s_pages.current == PAGE_SETTINGS
                       && views_settings_hit(c, tx, ty, &row, &choice)) {
                if (settings_select(&s_settings, row, choice)) {
                    settings_save(&s_settings);
                    apply_settings();
                }
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "tap at %d,%d -> setting %d = %d", tx, ty, row, choice);
            } else if (tx < c->w / 2) {
                /* The left half goes back, the right half forward. Buttons on
                   the settings page were tested first, so they still win. */
                page_t p = pages_back(&s_pages, now);
                ESP_LOGI(TAG, "tap at %d,%d -> back to page %d", tx, ty, (int)p);
            } else {
                page_t p = pages_advance(&s_pages, now);
                ESP_LOGI(TAG, "tap at %d,%d -> page %d", tx, ty, (int)p);
            }
            s_auto_jumped = false;          /* a tap means a person is choosing */
        }

        if (now - s_busy_check_us > BUSY_CHECK_US) {
            s_busy_check_us = now;
            usagedata_merge(&s_data, &v);
            bool busy = claude_busy(&v, now);
            if (busy && !s_busy && s_settings.auto_now
                && (s_pages.current == home_page() || s_saver)
                && s_pages.current != PAGE_NOW
                && (s_pages.available & PAGE_BIT(PAGE_NOW))) {
                pages_show(&s_pages, PAGE_NOW, now);
                s_auto_jumped = true;
                ESP_LOGI(TAG, "busy -> live page");
            } else if (!busy && s_busy && s_auto_jumped && s_pages.current == PAGE_NOW) {
                pages_show(&s_pages, home_page(), now);
                s_auto_jumped = false;
                ESP_LOGI(TAG, "idle -> home");
            }
            /* While busy on the live page, keep the saver away. */
            if (busy && s_auto_jumped && s_pages.current == PAGE_NOW)
                s_pages.last_activity_us = now;
            s_busy = busy;
        }

#if HAVE_LEVEL
        /* The level is a thing you are using, not a thing left on display, so
           the saver must not take it away underneath you -- and reading one
           sends the board nothing, which is exactly what looks like idling.
           It needs no protecting from burn-in either: the bubble moves. */
        if (s_pages.current == PAGE_LEVEL) s_pages.last_activity_us = now;
#endif
        bool saver_now = s_synced && pages_saver_active(&s_pages, now);
        if (saver_now != s_saver) {
            s_saver = saver_now;
            s_drawn_page = PAGE_COUNT;      /* force a full redraw either way */
            s_drawn_second = -1;
            s_saver_minute = -1;
            s_cycle_last = now;
            /* Start the slideshow by moving on, so it is visibly a saver. */
            if (s_saver && s_settings.saver_cycle) pages_step(&s_pages, cycle_skip());
        }
        if (s_saver && !s_settings.saver_cycle) {
            uint32_t secs = timecalc_advance(s_base_secs,
                                             (uint64_t)(now - s_base_us));
            if ((int)secs != s_drawn_second) {
                draw_saver(c, secs);
                s_drawn_second = (int)secs;
            }
            continue;
        }
        if (s_saver && now - s_cycle_last > (int64_t)s_settings.dwell_s * 1000000LL) {
            /* Cycling: step without touching the activity clock, so the
               saver stays on, and let the ordinary drawing below show it. */
            s_cycle_last = now;
            pages_step(&s_pages, cycle_skip());
        }

        /* Once idle, cycle the pages so no image sits long enough to burn in. */
        pages_tick(&s_pages, now);

        /* The strip in the title bars: local time and UTC, once both the
           board's clock and the Mac's offset are known. */
        {
            uint32_t secs = s_synced ? now_secs(now) : 0;
            vw_set_clock(s_synced && s_data.have_utc, secs, s_data.utc_offset_min, s_data.tz);
            int minute = s_synced ? (int)(secs / 60) : -1;
            if (minute != s_drawn_minute) {
                s_drawn_minute = minute;
                if (s_pages.current != PAGE_CLOCK && s_drawn_page == s_pages.current) {
                    s_drawn_page = PAGE_COUNT;
                    s_quiet_redraw = true;
                }
            }
        }

        const page_def_t *pd = &page_defs[s_pages.current];
        if (s_pages.current != s_drawn_page) {
            s_drawn_page = s_pages.current;
            s_drawn_second = -1;
            s_page_drawn_us = now;
            bool quiet = s_quiet_redraw;
            s_quiet_redraw = false;
            if (pd->draw && pd->animated && !quiet) {
                s_anim_start = now;      /* drawn by the animation below */
            } else if (pd->draw) {
                s_anim_start = 0;
                usagedata_merge(&s_data, &v);
                pd->draw(c, &v, 1.0f, now);
                display_blit();
            } else {
                /* The few pages that draw from something other than the
                   merged view. The clock draws itself below, every second. */
                switch (s_pages.current) {
                case PAGE_MESSAGE:  draw_message(c); display_blit(); break;
                case PAGE_TODAY:    views_today(c, &s_data); display_blit(); break;
                case PAGE_SETTINGS: views_settings(c, &s_settings); display_blit(); break;
                case PAGE_MENU:     views_menu(c, &s_pages); display_blit(); break;
                default: break;
                }
            }
        }
        /* Grow the bars into place, then hold the finished chart. */
        if (s_anim_start != 0 && pd->draw && pd->animated) {
            int64_t elapsed = now - s_anim_start;
            float t = (float)elapsed / (float)ANIM_US;
            bool last = t >= 1.0f;
            if (last) { t = 1.0f; s_anim_start = 0; }
            /* Ease out, so the bars settle rather than stopping dead. */
            t = 1.0f - (1.0f - t) * (1.0f - t);

            usagedata_merge(&s_data, &v);
            pd->draw(c, &v, t, now);
            display_blit();
        }

        if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);
#if HAVE_LEVEL
        if (s_pages.current == PAGE_LEVEL && s_imu) draw_level(c);
#endif
        /* Pages with ages on them redraw on their own so the ages keep counting. */
        if (pd->refresh_us > 0 && now - s_page_drawn_us > pd->refresh_us)
            s_drawn_page = PAGE_COUNT;

        /* USB-Serial-JTAG drops output when no host is attached, so the boot
           log is often missed. A heartbeat makes liveness observable. */
        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            ESP_LOGI(TAG, "alive, page %d, clock %s, rtc %s",
                     (int)s_pages.current, s_synced ? "synced" : "unset",
                     s_rtc ? "present" : "absent");
        }
    }
}
