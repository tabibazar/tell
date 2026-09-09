#include "display.h"
#include "ble_uart.h"
#include "gt911.h"
#include "i2cbus.h"
#include "bmp280.h"
#include "level.h"
#include "tiltgame.h"
#include "particles.h"
#include "qmi8658.h"
#include "pages.h"
#include "timecalc.h"
#include "usagedata.h"

#include "palette.h"
#include "views.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch poll interval */

/* True only where the screensaver draws something that does not need a
   synced clock. Defined here because the saver condition below uses it. */
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
#define SAVER_NEEDS_NO_CLOCK s_imu
#else
#define SAVER_NEEDS_NO_CLOCK false
#endif

static uint32_t s_base_secs;
static int64_t  s_base_us;
static bool     s_synced;

static usagedata_t s_data;
#define MESSAGE_MAX 512
static char s_message[MESSAGE_MAX + 1];
static pages_t s_pages;

/* Forces a redraw when the page or the displayed second changes. */
static page_t s_drawn_page = PAGE_COUNT;
static int s_drawn_second = -1;

/* Charts grow into place when a page appears; 0 means no animation running. */
#define ANIM_US (600 * 1000LL)
static int64_t s_anim_start = 0;

/* Screensaver: the clock drifts to a new spot each minute, so no pixel stays
   lit. Position is derived from the minute, so it is stable within one. */
static bool s_saver = false;
static int s_saver_minute = -1;
static int s_saver_x, s_saver_y;

/* Particles, on the Feather only, driven by its QMI8658. They are the
   screensaver there: a field in constant motion protects the panel better
   than a clock that moves once a minute. */
#ifdef CONFIG_SCREEN_BOARD_FEATHER_S3_TFT
#define HAVE_PARTICLES 1
static particles_t s_particles;
static bool s_imu;
static int64_t s_particles_last_us;
static float s_gx, s_gy;      /* low-passed gravity, panel coordinates */

/* Gravity in pixels per second squared. A full 1 g tilt crosses the short
   axis of the panel in about half a second, which reads as sand rather than
   as a screensaver. */
#define GRAVITY_PX 900.0f
/* First-order low pass, roughly a 150 ms time constant at 20 fps. Raw
   accelerometer output at rest is noisy enough to make a heap shiver. */
#define GRAVITY_ALPHA 0.25f
/* Degrees per second of twist, converted to a tangential nudge. */
#define SWIRL_SCALE 0.00015f

/*
 * Shaking is not a direction, it is energy, so it must not go through the
 * filter above. That filter has a corner around 1 Hz, which is what stops the
 * settled heap shivering on sensor noise -- but a real shake is five to ten
 * times faster, so the filter removes precisely the thing we want, and the
 * board ends up moving less the harder it is shaken.
 *
 * The residual is the other half of the same filter and has the opposite
 * response: near zero when the board is still, and near the full amplitude
 * when the reading is changing faster than the filter can follow. Feeding it
 * in as agitation scatters the pile instead of tilting it.
 */
#define SHAKE_FLOOR 250.0f     /* residual below this is noise, not a shake */
#define SHAKE_GAIN  0.06f      /* residual px/s^2 -> scatter px/s */
#define SHAKE_MAX   200.0f     /* enough to lift the pile, not to blur it */

/*
 * Which sensor axis is which on the panel, measured on the board rather than
 * assumed. An accelerometer at rest reads +1 g along the axis pointing UP, so
 * the gravity vector is minus the reading: G = -a.
 *
 * Standing on its long edge, screen upright, the board reads
 * (+0.99, +0.15, -0.20). So sensor +X points up, and the panel's down axis is
 * -X. That makes the downward component G . y = (-a) . (-X) = +ax.
 *
 * Laid flat instead, it read (+0.50, +0.12, -0.96): X and Z traded places
 * while Y barely moved, so Y is the axis it was rotated about -- the board's
 * long edge, which is the panel's horizontal. Z is therefore the screen's
 * normal and takes no part in this.
 *
 * That fixes both axes and the sign of the vertical one. The sign of the
 * horizontal is the one thing two still readings cannot give, since gravity
 * had no component along it either time -- it was read off the board instead,
 * by tipping it and seeing which way the sand went.
 */
#define AXIS_X (-1.0f)      /* panel +x, to the right, is sensor -y */
#define AXIS_Y ( 1.0f)      /* panel +y, downwards,    is sensor +x */

static void gravity_from(const qmi8658_sample_t *s, float *gx, float *gy)
{
    *gx = AXIS_X * s->ay;
    *gy = AXIS_Y * s->ax;
}

/* Roll is how far the panel is turned within its own plane, pitch how far
   its face is off vertical. Both come from the same gravity vector the liquid
   uses, so they inherit the axis mapping measured on the board. Z takes no
   part in the liquid but is exactly what pitch is made of. */
/* The level keeps its own filtered copy of gravity, in g and much slower than
   the liquid's. The liquid wants to feel the board move; an instrument wants
   to be read, and at the animation filter's speed the tenths digit never
   stops moving. This corner is about a fifth of a hertz -- it takes a second
   to catch up with a deliberate tilt and ignores everything faster. */
#define LEVEL_ALPHA 0.06f
static float s_lx, s_ly, s_lz;

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
    level_draw(c, asinf(ax) * 180.0f / (float)M_PI,
                  asinf(ay) * 180.0f / (float)M_PI);
    display_blit();
}

/* The game wants gravity as the liquid does -- quick, so the ball answers the
   board -- so it shares that filter rather than the level's slow one. */
static tiltgame_t s_game;
static int64_t s_game_last_us;

static void draw_game(canvas_t *c, int64_t now)
{
    float dt = s_game_last_us
             ? (float)(now - s_game_last_us) / 1000000.0f : 0.033f;
    s_game_last_us = now;
    if (dt > 0.2f) dt = 0.2f;

    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) == ESP_OK) {
        float gx, gy;
        gravity_from(&sample, &gx, &gy);
        s_gx += (gx * GRAVITY_PX - s_gx) * GRAVITY_ALPHA;
        s_gy += (gy * GRAVITY_PX - s_gy) * GRAVITY_ALPHA;
    }
    tiltgame_step(&s_game, s_gx, s_gy, dt);
    tiltgame_draw(&s_game, c);
    display_blit();
}

static void draw_particles(canvas_t *c, int64_t now)
{
    float dt = s_particles_last_us
             ? (float)(now - s_particles_last_us) / 1000000.0f : 0.05f;
    s_particles_last_us = now;
    if (dt > 0.2f) dt = 0.2f;     /* a long stall must not teleport anything */

    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) == ESP_OK) {
        float gx, gy;
        gravity_from(&sample, &gx, &gy);

        /* The filter's own error, before it is applied: how far the board is
           from where the slow view of gravity thinks it is. */
        float rx = gx * GRAVITY_PX - s_gx;
        float ry = gy * GRAVITY_PX - s_gy;

        s_gx += rx * GRAVITY_ALPHA;
        s_gy += ry * GRAVITY_ALPHA;

        float shake = sqrtf(rx * rx + ry * ry) - SHAKE_FLOOR;
        if (shake > 0.0f) {
            float scatter = shake * SHAKE_GAIN;
            if (scatter > SHAKE_MAX) scatter = SHAKE_MAX;
            particles_agitate(&s_particles, scatter);
        }
        /* Spinning the board stirs the pile. Garnish; nothing depends on it. */
        particles_swirl(&s_particles, sample.gz * SWIRL_SCALE);
    }

    particles_step(&s_particles, s_gx, s_gy, dt);
    particles_draw(&s_particles, c);
    display_blit();
}
#else
#define HAVE_PARTICLES 0
#endif

static void on_time(uint32_t secs)
{
    s_base_secs = secs;
    s_base_us = esp_timer_get_time();
    s_synced = true;
    s_drawn_second = -1;
}

static void on_message(const char *text, size_t len)
{
    int64_t now = esp_timer_get_time();

    ud_kind_t kind = usagedata_parse(&s_data, text, now);
    if (kind == UD_GAME) {
#if HAVE_PARTICLES
        /* A fresh round whenever it is asked for, so "!game" always starts
           one rather than dropping you into a run already lost. */
        tiltgame_restart(&s_game);
        pages_show(&s_pages, PAGE_GAME, now);
        s_drawn_page = PAGE_COUNT;
#endif
        return;
    }
    if (kind == UD_LEVEL) {
#if HAVE_PARTICLES
        pages_show(&s_pages, PAGE_LEVEL, now);
        s_drawn_page = PAGE_COUNT;
#endif
        return;
    }
    if (kind == UD_PARTICLES) {
#if HAVE_PARTICLES
        /* Unlike the data markers this is a request, so it does take the
           view: someone asked to see it. */
        pages_show(&s_pages, PAGE_PARTICLES, now);
        s_drawn_page = PAGE_COUNT;
#endif
        return;
    }
    if (kind == UD_TODAY) {
        if (s_pages.current == PAGE_TODAY) s_drawn_page = PAGE_COUNT;
        return;
    }
    if (kind == UD_CLOCK) {
        /* Arrives on a timer like the charts, so it must not steal the view. */
        if (s_pages.current == PAGE_CLOCK) s_drawn_second = -1;
        return;
    }
    if (kind == UD_STATS || kind == UD_DAILY) {
        /* Data arrives on a timer, so it must never steal the view: refresh
           the numbers, and redraw only if that page is already showing. */
        page_t target = (kind == UD_STATS) ? PAGE_STATS : PAGE_DAILY;
        if (s_pages.current == target) s_drawn_page = PAGE_COUNT;
        return;
    }
    if (len == 0) {
        s_message[0] = '\0';
        pages_show(&s_pages, PAGE_CLOCK, now);
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

    canvas_t *c = display_canvas();

    unsigned available = PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE);
    bool touch = false;
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    available |= PAGE_BIT(PAGE_STATS) | PAGE_BIT(PAGE_DAILY)
               | PAGE_BIT(PAGE_TODAY);
    touch = gt911_init() == ESP_OK;
#endif
#if HAVE_PARTICLES
    s_imu = qmi8658_init() == ESP_OK;
    if (s_imu) {
        available |= PAGE_BIT(PAGE_PARTICLES) | PAGE_BIT(PAGE_LEVEL)
                   | PAGE_BIT(PAGE_GAME);
        /* Seeded from the pressure sensor's noise rather than a constant, so
           the grains do not land in the same places every boot. Its bottom
           bits wander on their own; the boot timer, by contrast, reads much
           the same every time the board is reset the same way. */
        uint32_t seed = 0;
        if (bmp280_init() == ESP_OK) seed = bmp280_entropy();
        if (seed == 0) seed = (uint32_t)esp_timer_get_time() | 1u;
        ESP_LOGI(TAG, "particles seeded with 0x%08X", (unsigned)seed);
        particles_init(&s_particles, 190, c->w, c->h, seed);
        tiltgame_init(&s_game, c->w, c->h, seed ^ 0x5A5A5A5Au);
    }
#endif
    pages_init(&s_pages, available);

    if (ble_uart_start(on_message, on_time) != ESP_OK) {
        ESP_LOGE(TAG, "ble start failed");
        canvas_text(c, "BLE FAILED");
        display_blit();
        return;
    }

    int64_t last_beat = 0;
    int64_t last_wake = esp_timer_get_time();

    for (;;) {
        /* Sleep for what is left of the frame, not for a whole frame on top
           of the work. vTaskDelay is time added after everything else has
           run, so sleeping a flat 33 ms after 13 ms of solving and blitting
           gives 23 fps, not 30 -- which is most of why the liquid looked
           slow. The tick is 10 ms, so this lands on the nearest tick below. */
        int64_t period_us = (int64_t)TICK_MS * 1000;
#if HAVE_PARTICLES
        if (s_imu && (s_saver || s_pages.current == PAGE_PARTICLES
                              || s_pages.current == PAGE_LEVEL
                              || s_pages.current == PAGE_GAME))
            period_us = 33000;      /* 30 fps while the liquid is showing */
#endif
        int64_t rest_ms = (period_us - (esp_timer_get_time() - last_wake)) / 1000;
        vTaskDelay(rest_ms > 1 ? pdMS_TO_TICKS(rest_ms) : 1);

        int64_t now = esp_timer_get_time();
        last_wake = now;

        if (touch && gt911_tapped()) {
            if (s_saver) {
                /* The first tap dismisses the saver rather than also changing
                   the page, which would be a surprise. */
                s_pages.last_activity_us = now;
                ESP_LOGI(TAG, "tap -> wake");
            } else {
                page_t p = pages_advance(&s_pages, now);
                ESP_LOGI(TAG, "tap -> page %d", (int)p);
            }
        }

        /* The clock screensaver needs the time; the particles do not, so on
           a board with an IMU the saver runs whether or not a Mac has ever
           connected. */
        bool saver_now = pages_saver_active(&s_pages, now)
                       && (s_synced || SAVER_NEEDS_NO_CLOCK);
        if (saver_now != s_saver) {
            s_saver = saver_now;
            s_drawn_page = PAGE_COUNT;      /* force a full redraw either way */
            s_drawn_second = -1;
            s_saver_minute = -1;
        }
        if (s_saver) {
#if HAVE_PARTICLES
            if (s_imu) { draw_particles(c, now); continue; }
#endif
            uint32_t secs = timecalc_advance(s_base_secs,
                                             (uint64_t)(now - s_base_us));
            if ((int)secs != s_drawn_second) {
                draw_saver(c, secs);
                s_drawn_second = (int)secs;
            }
            continue;
        }

        /* Once idle, cycle the pages so no image sits long enough to burn in. */
        pages_tick(&s_pages, now);

        if (s_pages.current != s_drawn_page) {
            s_drawn_page = s_pages.current;
            s_drawn_second = -1;
            switch (s_pages.current) {
            case PAGE_STATS:
            case PAGE_DAILY:
                s_anim_start = now;      /* drawn by the animation below */
                break;
            case PAGE_MESSAGE: draw_message(c); display_blit(); break;
            case PAGE_TODAY:   views_today(c, &s_data); display_blit(); break;
            default: break;
            }
        }
        /* Grow the bars into place, then hold the finished chart. */
        if (s_anim_start != 0
            && (s_pages.current == PAGE_STATS || s_pages.current == PAGE_DAILY)) {
            int64_t elapsed = now - s_anim_start;
            float t = (float)elapsed / (float)ANIM_US;
            bool last = t >= 1.0f;
            if (last) { t = 1.0f; s_anim_start = 0; }
            /* Ease out, so the bars settle rather than stopping dead. */
            t = 1.0f - (1.0f - t) * (1.0f - t);

            ud_view_t v;
            usagedata_merge(&s_data, &v);
            if (s_pages.current == PAGE_STATS) views_stats(c, &v, t, now);
            else views_daily(c, &v, t, now);
            display_blit();
        }

        if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);
#if HAVE_PARTICLES
        if (s_pages.current == PAGE_PARTICLES && s_imu) draw_particles(c, now);
        if (s_pages.current == PAGE_LEVEL && s_imu) draw_level(c);
        if (s_pages.current == PAGE_GAME && s_imu) draw_game(c, now);
#endif

        /* USB-Serial-JTAG drops output when no host is attached, so the boot
           log is often missed. A heartbeat makes liveness observable. */
        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            ESP_LOGI(TAG, "alive, page %d, clock %s",
                     (int)s_pages.current, s_synced ? "synced" : "unset");
        }
    }
}
