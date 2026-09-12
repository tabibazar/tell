#include "display.h"
#include "drift.h"
#include "ble_uart.h"
#include "bme280.h"
#include "buttons.h"
#include "gt911.h"
#include "level.h"
#include "particles.h"
#include "qmi8658.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "particles.h"
#include "pagedefs.h"
#include "pages.h"
#include "view_common.h"
#include "ds3231.h"
#include "settings.h"
#include "shaketimer.h"
#include "templog.h"
#include "tempsense.h"
#include "timecalc.h"
#include "usagedata.h"

#include "palette.h"
#include "views.h"

#include "esp_log.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch and button poll interval */

/*
 * Thousandths on the clock. They cost a redraw every frame instead of every
 * second, which is most of what this board does while the clock is showing,
 * so it is a choice rather than a given.
 */
#define CLOCK_MILLISECONDS 1

/* Push buttons, where the board has any: on a board with no touch they are
   the only way to change the page, so they matter more there than a tap does
   on the big one. Which pins, and how many, is a Kconfig question. */
#if CONFIG_SCREEN_BUTTON_A >= 0
#define HAVE_BUTTONS 1
#else
#define HAVE_BUTTONS 0
#endif


static uint32_t s_base_secs;
static int64_t  s_base_us;
static bool     s_synced;

/* The battery-backed clock, when one is on the bus. A Mac's sync is written
   to it from the main loop rather than from the BLE task that receives it,
   so the I2C bus is only ever driven from one task. */
static bool     s_rtc;
static bool     s_rtc_pending;      /* a sync arrived; copy it to the chip */
static int64_t  s_rtc_checked_us;
static uint32_t s_last_day_secs;    /* to notice our own clock crossing midnight */
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

/*
 * Anything that needs the IMU. Two boards have one: the Feather, where a
 * QMI8658 hangs off the STEMMA QT port, and wave, where the same chip is
 * soldered to the board at GPIO48/47. lilly and the CrowPanel have no sensor
 * and are compiled with none of this.
 */
#if defined(CONFIG_SCREEN_BOARD_FEATHER_S3_TFT) \
  || defined(CONFIG_SCREEN_BOARD_WAVESHARE_147B)
#define HAVE_IMU 1
#else
#define HAVE_IMU 0
#endif

/* What each board does with it. Both pour the grains; only the Feather also
   reads the sensor as an instrument. The axis mapping below is shared,
   because it describes the sensor rather than either page -- which is exactly
   why the sand has to negate one component of it: a bubble floats against
   gravity and grains fall with it. */
/* Both sensor boards read the level; it is the same instrument either way. */
#define HAVE_LEVEL HAVE_IMU

/*
 * One convention, everywhere: gravity_from gives the direction things fall.
 *
 * The sand takes it as it comes. The level turns it over, because a bubble
 * floats away from gravity while grains fall towards it -- a fixed, physical
 * disagreement, and the only sensible place for it is in the page that
 * disagrees rather than in the mapping both of them share.
 *
 * This used to be muddled: the sand negated and the level did not, which
 * meant the stored mapping meant different things on different boards
 * depending on which page it had been calibrated against. With one
 * convention a board is calibrated once, with "!flip", and both pages agree.
 */
/* The grains pour on either board that has the sensor. */
#define HAVE_PARTICLES HAVE_IMU

#if HAVE_IMU
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
#endif /* HAVE_IMU */

#if HAVE_LEVEL

/* The level keeps its own filtered copy of gravity, in g and much slower than
   the liquid's. The liquid wants to feel the board move; an instrument wants
   to be read, and at the animation filter's speed the tenths digit never
   stops moving. This corner is about a fifth of a hertz -- it takes a second
   to catch up with a deliberate tilt and ignores everything faster.

   Slower still since the dial went to five degrees full scale, where every
   twitch of a hand is eleven pixels of dot: the reading was honest and
   unwatchable. But not as slow as it briefly was, because smoothing the dot
   also smooths away the tremor that should be ending a run, and a level that
   hides your mistakes is both a worse instrument and an easier game than it
   ought to be. */
#define LEVEL_ALPHA 0.04f
static float s_lx, s_ly, s_lz;

/*
 * The clock only runs once the board has actually been picked up, because a
 * board resting on a level desk holds true for ever and would take the record
 * by being left alone -- which it had already done, with twenty-four seconds
 * nobody earned.
 *
 * Telling a held board from a resting one by how much the reading trembles
 * needs a threshold between a hand and a table, and two attempts at guessing
 * that threshold were both wrong: too low and the desk scored, too high and a
 * real hand could not arm it. But the question answers itself. A board lying
 * on a level surface never leaves level. So a run may only begin after the
 * board has been off level at least once since the page opened -- which a
 * resting board never manages, and which anyone picking it up does without
 * trying. No threshold, and nothing to measure.
 */
static bool s_armed;

/* The surface the board is standing on, as captured by "!zero": angles are
   reported relative to it. A desk is not a reference plane and neither is a
   sensor soldered by hand, so "level" is most usefully "level with whatever
   this is sitting on now". "!zero reset" goes back to absolute. */
static float s_zero_x, s_zero_y;
static bool  s_zeroed;

/* Holding a board flat by hand is harder than it sounds, so the level keeps a
   clock: how long it has been true for, the run that just ended, and the
   longest there has ever been. Both times outlive the power, so whoever picks
   the board up next inherits a mark to beat.

   They are written when a run ends and never while one is running -- at
   thirty frames a second that would be thousands of flash writes a minute --
   and only for runs that were actually attempts. Carrying the board past
   level trips the tolerance for a fraction of a second on the way through,
   and without a floor every one of those would count as somebody's turn and
   cost a write. */
#define LEVEL_MIN_RUN_S 1.0f
static float s_hold_s, s_last_hold_s, s_prev_hold_s, s_best_hold_s;
static int64_t s_level_last_us;

static void runs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "hold", &s_best_hold_s, sizeof s_best_hold_s);
    nvs_set_blob(h, "last", &s_last_hold_s, sizeof s_last_hold_s);
    nvs_set_blob(h, "prev", &s_prev_hold_s, sizeof s_prev_hold_s);
    nvs_commit(h);
    nvs_close(h);
}

static void zero_load(void)
{
    nvs_handle_t h;
    if (nvs_open("screen", NVS_READONLY, &h) != ESP_OK) return;
    size_t hn = sizeof(float);
    nvs_get_blob(h, "hold", &s_best_hold_s, &hn);
    hn = sizeof(float);
    nvs_get_blob(h, "last", &s_last_hold_s, &hn);
    hn = sizeof(float);
    nvs_get_blob(h, "prev", &s_prev_hold_s, &hn);
    size_t n = sizeof(float);
    if (nvs_get_blob(h, "zerox", &s_zero_x, &n) == ESP_OK) {
        n = sizeof(float);
        if (nvs_get_blob(h, "zeroy", &s_zero_y, &n) == ESP_OK) s_zeroed = true;
    }
    nvs_close(h);
    if (s_zeroed)
        ESP_LOGI(TAG, "level zeroed at %+.3f %+.3f",
                 (double)s_zero_x, (double)s_zero_y);
    ESP_LOGI(TAG, "level times: last %.1fs, prev %.1fs, best %.1fs",
             (double)s_last_hold_s, (double)s_prev_hold_s,
             (double)s_best_hold_s);
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
    /* The bubble floats to the high side, so it moves against gravity. */
    gx = -gx;
    gy = -gy;
    float gz = -sample.az;              /* out of the screen, toward you */

    int64_t now = esp_timer_get_time();
    float dt = s_level_last_us
             ? (float)(now - s_level_last_us) / 1000000.0f : 0.0f;
    bool fresh = dt <= 0.0f || dt > 0.5f;
    s_level_last_us = now;

    if (fresh) {
        /* Arriving on the page. Snap the filter to what the board is actually
           reading rather than letting it converge from zero: while it
           converges the panel is nowhere near level, which armed the game and
           then scored the settling as a sixteen-second run. */
        s_lx = gx; s_ly = gy; s_lz = gz;
        s_armed = false;
        s_hold_s = 0.0f;
        dt = 0.0f;
    } else {
        s_lx += (gx - s_lx) * LEVEL_ALPHA;
        s_ly += (gy - s_ly) * LEVEL_ALPHA;
        s_lz += (gz - s_lz) * LEVEL_ALPHA;
    }

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
    if (!level_is_true(tx, ty)) s_armed = true;

    if (level_is_true(tx, ty) && s_armed) {
        /* The best is not raised while the run is still going. It is the best
           *finished* run, which is what is written to flash; raising it live
           would show a record on screen that a sub-second run never earned
           and that a reboot would take away again. */
        s_hold_s += dt;
    } else if (s_hold_s > 0.0f) {
        /* A run just ended. This is the one moment worth a flash write, and
           only if it was long enough to have been someone trying. */
        if (s_hold_s >= LEVEL_MIN_RUN_S) {
            /* The two most recent attempts, newest first, so a second player
               can see what the one before them managed. */
            s_prev_hold_s = s_last_hold_s;
            s_last_hold_s = s_hold_s;
            if (s_hold_s > s_best_hold_s) s_best_hold_s = s_hold_s;
            runs_save();
            ESP_LOGI(TAG, "held %.1fs (prev %.1f, best %.1f)",
                     (double)s_hold_s, (double)s_prev_hold_s,
                     (double)s_best_hold_s);
        }
        s_hold_s = 0.0f;
    }

    /* One letter, bottom right: z means the angles are relative to a surface
       taken as true with "!zero", nothing means they are absolute. */
    level_draw(c, tx, ty, s_hold_s, s_last_hold_s, s_prev_hold_s,
               s_best_hold_s, s_armed);
    display_blit();
}
#endif /* HAVE_LEVEL */

#if HAVE_PARTICLES
/*
 * The sand: a bottle of grains that pour towards whichever way the board is
 * actually tilted. The physics is in particles.c, which knows nothing about
 * sensors or panels and is tested on the host; this is only the wiring from
 * one to the other. It runs on the Feather at 240x135 and on lilly at
 * 320x170, which is why everything below is scaled per panel.
 *
 * Static, not on the stack: a few hundred grains with their bucket grid is
 * tens of kilobytes, and the main task's stack is small.
 */
static particles_t s_particles;
static int64_t s_particles_last_us;
static float s_gx, s_gy;      /* low-passed gravity, panel coordinates */

/*
 * Gravity, the shake floor and the shake ceiling are all per panel row rather
 * than absolute, so the liquid behaves the same on any size of screen. The
 * numbers are the ones tuned by eye on the Feather's 135-row panel -- 900,
 * 250 and 200 -- divided by 135. On lilly's 170 rows that comes out at about
 * 1130, 315 and 250: a taller bottle needs proportionally stronger gravity to
 * fall through it in the same time, or it reads as smoke.
 */
#define GRAVITY_PER_ROW    6.67f    /* px/s^2 per row: 1 g crosses in ~0.5 s */
/* What the solver can actually settle. lilly's pile was tested at 1134 and
   stood twenty-four rows deep; past that a grain travels further per frame
   than the separation passes can correct, the body never comes to rest, and
   the whole pile shimmers. No panel here reaches it -- wave's 172 rows ask
   for 1147 -- but a taller one would, and the failure looks like a display
   fault rather than a physics one. */
#define GRAVITY_MAX     1200.0f
#define SHAKE_FLOOR_PER_ROW 1.85f   /* residual below this is noise */
#define SHAKE_MAX_PER_ROW   1.48f   /* enough to lift the pile, not blur it */
static float s_gravity_px, s_shake_floor, s_shake_max;

/* First-order low pass, roughly a 150 ms time constant at 20 fps. Raw
   accelerometer output at rest is noisy enough to make a heap shiver. */
#define GRAVITY_ALPHA 0.25f
/* Degrees per second of twist, converted to a tangential nudge. */
#define SWIRL_SCALE 0.00015f
/* Residual px/s^2 -> scatter px/s. Unitless, so it does not scale. */
#define SHAKE_GAIN 0.06f

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
        /* Falls the way gravity points, which is what the mapping means. */

        /* The filter's own error, before it is applied: how far the board is
           from where the slow view of gravity thinks it is. */
        float rx = gx * s_gravity_px - s_gx;
        float ry = gy * s_gravity_px - s_gy;

        s_gx += rx * GRAVITY_ALPHA;
        s_gy += ry * GRAVITY_ALPHA;

        /*
         * Shaking is not a direction, it is energy, so it must not go through
         * the filter above. That filter has a corner around 1 Hz, which is
         * what stops the settled heap shivering on sensor noise -- but a real
         * shake is five to ten times faster, so the filter removes precisely
         * the thing we want, and the board ends up moving less the harder it
         * is shaken. The residual is the other half of the same filter and
         * has the opposite response.
         */
        float shake = sqrtf(rx * rx + ry * ry) - s_shake_floor;
        if (shake > 0.0f) {
            float scatter = shake * SHAKE_GAIN;
            if (scatter > s_shake_max) scatter = s_shake_max;
            particles_agitate(&s_particles, scatter);
        }
        /* Spinning the board stirs the pile. Garnish; nothing depends on it. */
        particles_swirl(&s_particles, sample.gz * SWIRL_SCALE);
    }

    particles_step(&s_particles, s_gx, s_gy, dt);
    particles_draw(&s_particles, c);
    display_blit();
}

/*
 * A countdown with one control: shake it and it starts again from the top.
 *
 * It replaces an hourglass that wanted the board stood on its edge and turned
 * over, which is a lot to ask of something that lives flat on a desk. A shake
 * needs no posture at all, and shakedet reads energy rather than direction,
 * so it works lying down, in one hand, at any angle.
 */
#define TIMER_DEFAULT_S (25 * 60)

/*
 * Dialling the duration by hand.
 *
 * Tilt is a shuttle: the steeper it is held the faster the time runs, either
 * way, squared so that a small lean crawls and a hard one covers the whole
 * range in seconds. Four minutes a second held right over crosses 99 minutes
 * in about twenty-five, and a gentle lean moves it a second at a time.
 *
 * Panning stays proportional, because the gesture already carries its own
 * rate: turn the board faster and it winds faster, which is how a knob
 * behaves.
 */
#define DIAL_SECONDS_MAX        240.0f   /* seconds per second, held right over */
#define DIAL_TILT_DEADZONE        0.20f  /* g */
#define DIAL_DEGREES_PER_MINUTE 360.0f
#define DIAL_PAN_DEADZONE        25.0f   /* degrees per second */

static shaketimer_t s_timer;
static stopwatch_t s_watch;
static shakedet_t s_shake;
static int64_t s_timer_last_us;

/*
 * Counting up. A shake starts and stops it, as on the timer page: the shake
 * is what acts on the clock, and leaving the button alone means a tap still
 * turns the page. Holding the button puts it back to zero.
 */
static void draw_stopwatch(canvas_t *c, int64_t now)
{
    static int64_t last_us;
    float dt = last_us ? (float)(now - last_us) / 1000000.0f : 0.05f;
    last_us = now;
    if (dt > 0.5f) dt = 0.5f;

    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) == ESP_OK
        && shakedet_update(&s_shake, sample.ax, sample.ay, sample.az, dt, now)) {
        stopwatch_toggle(&s_watch);
        ESP_LOGI(TAG, "stopwatch %s at %.1f s",
                 stopwatch_running(&s_watch) ? "running" : "stopped",
                 (double)stopwatch_elapsed_s(&s_watch));
    }
    stopwatch_tick(&s_watch, dt);

    /* Tenths, not hundredths. The page redraws at the tick, so a hundredths
       digit would step in fives and read as noise; a tenth is a digit that
       means what it says. */
    float e = stopwatch_elapsed_s(&s_watch);
    int total = (int)e;
    int tenths = (int)((e - (float)total) * 10.0f);
    char buf[16];
    if (total >= 3600)
        snprintf(buf, sizeof buf, "%d:%02d:%02d", total / 3600,
                 (total / 60) % 60, total % 60);
    else
        snprintf(buf, sizeof buf, "%d:%02d.%d", total / 60, total % 60, tenths);

    const char *note = stopwatch_running(&s_watch) ? NULL
                     : (e > 0.0f ? "SHAKE TO GO ON  HOLD TO CLEAR"
                                 : "SHAKE TO START");
    canvas_clear(c);
    canvas_big(c, buf);
    if (note) canvas_puts(c, (c->cols - (int)strlen(note)) / 2, c->rows - 1,
                          note, PAL_DIM);
    display_blit();
}

static void draw_timer(canvas_t *c, int64_t now)
{
    float dt = s_timer_last_us
             ? (float)(now - s_timer_last_us) / 1000000.0f : 0.05f;
    s_timer_last_us = now;
    if (dt > 0.5f) dt = 0.5f;

    qmi8658_sample_t sample;
    bool have = qmi8658_read(&sample) == ESP_OK;

    if (have && shaketimer_setting(&s_timer)) {
        /*
         * Dialling. Two gestures, two scales, both as rates rather than
         * positions so letting go leaves the value where it is.
         *
         * Panning -- turning the board flat, like a knob -- is the big,
         * comfortable motion, so it carries minutes: a full turn is one. It
         * comes off the gyroscope, which nothing else here uses for anything
         * that matters. Tilting is the smaller motion and carries seconds.
         *
         * Both have a dead zone, because a hand holding a board is never
         * quite still and a dial that creeps while you read it is useless.
         */
        float gx, gy;
        gravity_from(&sample, &gx, &gy);
        float pan = sample.gz;                   /* degrees per second */
        if (pan > -DIAL_PAN_DEADZONE && pan < DIAL_PAN_DEADZONE) pan = 0.0f;
        float seconds = shaketimer_shuttle(gx, DIAL_TILT_DEADZONE, DIAL_SECONDS_MAX);
        shaketimer_adjust(&s_timer, pan / DIAL_DEGREES_PER_MINUTE, seconds, dt);
    } else if (have && shakedet_update(&s_shake, sample.ax, sample.ay, sample.az,
                                       dt, now)) {
        shaketimer_shake(&s_timer);
        ESP_LOGI(TAG, "shaken: %d s from the top", s_timer.duration_s);
    }
    shaketimer_tick(&s_timer, dt);

    int left = (int)(shaketimer_remaining_s(&s_timer) + 0.5f);
    char buf[16];
    snprintf(buf, sizeof buf, "%d:%02d", left / 60, left % 60);

    const char *note;
    uint16_t colour;
    switch (shaketimer_state(&s_timer)) {
    case ST_RUNNING: note = NULL;             colour = PAL_FG;  break;
    case ST_SETTING: note = "TILT TO SET, TAP"; colour = PAL_A0;  break;
    /* Done blinks, because the whole point is to be noticed from across a
       room by someone who has stopped looking at it. */
    case ST_DONE:    note = "TIME";           colour = PAL_A5;  break;
    default:         note = "SHAKE TO START"; colour = PAL_DIM; break;
    }

    /* canvas_big always paints in the foreground colour, so the state is
       carried by the note under the digits rather than by their colour --
       and when the time is up the digits blink, which is louder than any
       colour and is the one moment this page needs to be noticed. */
    canvas_clear(c);
    bool dark = shaketimer_state(&s_timer) == ST_DONE && (now / 500000) % 2;
    if (!dark) canvas_big(c, buf);
    if (note) canvas_puts(c, (c->cols - (int)strlen(note)) / 2, c->rows - 1,
                          note, colour);
    display_blit();
}
#endif /* HAVE_PARTICLES */

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
    /* On a board whose only reason to have a screen is the level, the level
       is home. The big board keeps the clock: it is a display of Claude's
       usage that happens to know the time, and this one is an instrument. */

    /* And a board whose sensor exists to pour sand rests on the sand. The
       level is checked first, so the Feather, which has both, still comes
       home to the instrument rather than to the toy. This matters most on a
       board with no RTC: the clock it would otherwise show after every power
       cycle reads --:--:-- until a Mac speaks to it. */
    if (s_pages.available & PAGE_BIT(PAGE_PARTICLES)) return PAGE_PARTICLES;
    if (s_pages.available & PAGE_BIT(PAGE_LEVEL)) return PAGE_LEVEL;
    if (s_settings.home_now && (s_pages.available & PAGE_BIT(PAGE_NOW))) return PAGE_NOW;
    return PAGE_CLOCK;
}

/*
 * Pages the screensaver has been told to leave out, one bit each, chosen by
 * double-tapping a tile on the menu and kept in flash.
 *
 * A double tap costs the single tap its immediacy: opening a page has to wait
 * MENU_DOUBLE_US to find out whether a second tap is coming. That delay is
 * paid only on the menu, and only because the alternative -- opening the page
 * and then undoing it when the second tap lands -- would flash a page nobody
 * asked for. The tile lights up under the finger as soon as it is touched, so
 * the wait is visible rather than felt as lag.
 */
static uint32_t s_cycle_off;
#define MENU_DOUBLE_US 400000
static page_t s_menu_held = PAGE_COUNT;   /* the tile waiting to see a second tap */
static int64_t s_menu_held_us;

/* The pages the cycling saver leaves out: settings, because a slideshow
   should not land on a control panel, the message page when nothing has been
   sent, because "nothing sent yet" is not worth twenty seconds, and whatever
   has been struck off on the menu. */
static unsigned cycle_skip(void)
{
    unsigned skip = s_cycle_off;
    for (int i = 0; i < PAGE_COUNT; i++)
        if (!page_defs[i].in_saver) skip |= PAGE_BIT(i);
    if (s_message[0] == '\0') skip |= PAGE_BIT(PAGE_MESSAGE);
    return skip;
}

/* A tile's second tap: strike the page off the saver's round, or put it
   back. Only pages the saver would visit anyway can be struck off -- the
   menu and the settings page are never in it, so double-tapping them would
   set a bit that changes nothing and show a mark that means nothing. */
static void toggle_cycle(page_t page)
{
    if (!page_defs[page].in_saver) {
        ESP_LOGI(TAG, "page %d is never in the saver; nothing to skip", (int)page);
        return;
    }
    s_cycle_off ^= PAGE_BIT(page);
    settings_save_cycle_off(s_cycle_off);
    ESP_LOGI(TAG, "page %d %s the saver", (int)page,
             (s_cycle_off & PAGE_BIT(page)) ? "struck off" : "back in");
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
        if (s_rtc) {
            /* A date has to ask for the chip to be written in its own right.
               The time arrives first, as its own sync, and the write that
               follows it happens on the next tick -- before this payload has
               been read. A date that waited for the time's write would miss
               it by one message, every time. */
            ds3231_date_t d = { s_data.date_year, s_data.date_month,
                                s_data.date_day, s_data.date_wday };
            if (ds3231_date_valid(&d)) s_rtc_pending = true;
        }
        return;
    }
#if HAVE_IMU
    /* Commands, not data: unlike the charts these were asked for, so they do
       take the view. "!flip" belongs to whichever IMU page this board has,
       because the axes it corrects are the sensor's, not the page's. */
    if (kind == UD_FLIP) axis_command(text);
#endif
    if (kind == UD_PAGE) {
        /*
         * "!page clock" and the like. A board with no touch and no buttons
         * has no other way to choose what it shows: the markers that exist
         * each open one particular page, and the rest were unreachable.
         * Matched against the names already in pagedefs, so a new page is
         * reachable the moment it has a row there.
         */
        char want[64];
        size_t n = 0;
        for (const char *p = text; *p && n + 1 < sizeof want; p++)
            want[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
        want[n] = '\0';

        for (int i = 0; i < PAGE_COUNT; i++) {
            if (!(s_pages.available & PAGE_BIT(i))) continue;
            char name[32];
            size_t m = 0;
            for (const char *p = page_defs[i].name; *p && m + 1 < sizeof name; p++)
                name[m++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
            name[m] = '\0';
            if (m == 0 || strstr(want, name) == NULL) continue;
            pages_show(&s_pages, (page_t)i, now);
            s_drawn_page = PAGE_COUNT;
            ESP_LOGI(TAG, "showing %s", page_defs[i].name);
            return;
        }
        ESP_LOGW(TAG, "no page here matches \"%s\"", text);
        return;
    }
#if HAVE_PARTICLES
    if (kind == UD_STOPWATCH) {
        pages_show(&s_pages, PAGE_STOPWATCH, now);
        s_drawn_page = PAGE_COUNT;
        return;
    }
    if (kind == UD_TIMER) {
        long mins = 0;
        for (const char *p = text; *p; p++)
            if (*p >= '0' && *p <= '9') { mins = strtol(p, NULL, 10); break; }
        if (mins > 0 && mins <= 24 * 60) shaketimer_init(&s_timer, (int)mins * 60);
        pages_show(&s_pages, PAGE_TIMER, now);
        s_drawn_page = PAGE_COUNT;
        return;
    }
    if (kind == UD_PARTICLES || kind == UD_FLIP) {
        pages_show(&s_pages, PAGE_PARTICLES, now);
        s_drawn_page = PAGE_COUNT;
        return;
    }
#endif
#if HAVE_LEVEL
    if (kind == UD_LEVEL || kind == UD_FLIP || kind == UD_ZERO
     || kind == UD_NEWGAME) {
        if (kind == UD_ZERO) zero_command(text);
        if (kind == UD_NEWGAME) {
            /* Wipe the scoreboard. A score set before the clock knew to ask
               whether anyone was holding the board is not one anybody made,
               and there was no way to clear it without a reflash. */
            s_hold_s = s_last_hold_s = s_prev_hold_s = s_best_hold_s = 0.0f;
            runs_save();
            ESP_LOGI(TAG, "scoreboard cleared");
        }
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

/*
 * Takes the day from the chip. The board cannot work out the date for itself
 * -- it counts seconds since midnight and nothing else -- so the calendar is
 * the chip's to keep and ours to ask for.
 */
/*
 * The two temperatures, sampled on a timer whether or not anyone is looking.
 * A chart that only filled while its page was open would be empty every time
 * you went to it, which is the opposite of what a chart is for.
 */
/*
 * Five minutes. Two seconds charted the last five minutes of a board that
 * barely changes, which is the least interesting window there is; at five
 * minutes the hundred and sixty samples cover about thirteen hours, so the
 * page shows a night's worth of the room warming and cooling around the
 * board and the board's own load moving under it.
 */
#define TEMPLOG_EVERY_S  (5 * 60)
#define TEMPLOG_EVERY_US ((int64_t)TEMPLOG_EVERY_S * 1000000LL)
static templog_t s_templog;
static int64_t s_templog_us;

static void templog_sample(int64_t now)
{
    if (s_templog_us != 0 && now - s_templog_us < TEMPLOG_EVERY_US) return;
    s_templog_us = now;
    float die = 0.0f, xtal = 0.0f;
    if (!tempsense_read(&die)) return;
    templog_add(&s_templog, die, ds3231_temperature(&xtal), xtal);
    if (s_pages.current == PAGE_TEMPS) s_drawn_page = PAGE_COUNT;
}

static void rtc_refresh_date(void)
{
    ds3231_date_t d;
    if (!s_rtc || !ds3231_read_date(&d)) return;
    char was[sizeof s_data.date];
    strncpy(was, s_data.date, sizeof was - 1);
    was[sizeof was - 1] = '\0';
    ds3231_format_date(&d, s_data.date, (int)sizeof s_data.date);
    s_data.date_year = d.year; s_data.date_month = d.month;
    s_data.date_day = d.day;   s_data.date_wday = d.wday;
    if (strcmp(was, s_data.date) != 0) {
        ESP_LOGI(TAG, "date from the RTC: %s", s_data.date);
        if (s_pages.current == PAGE_CLOCK) s_drawn_second = -1;
    }
}

/*
 * The clock chip, and what it says about itself.
 *
 * The number that matters is the drift: the board's own sense of time comes
 * from the ESP timer, which is a bare crystal and wanders seconds a day,
 * while the DS3231 compensates itself against its own temperature and holds
 * a couple of parts per million. Showing both and the gap between them says
 * how much the board would have been wrong by without the chip.
 */
/*
 * The clock chip, and how far the board has slipped against it.
 *
 * Charting "drift" the obvious way -- the chip against the board, in seconds
 * -- would draw a flat line at zero for a day: a DS3231 is good to about a
 * sixth of a second in that time. So the chart runs the comparison the other
 * way round, which is the way that moves. See drift.h.
 *
 * Sampling happens whether or not this page is showing, so turning to it
 * shows a chart rather than an empty one filling up.
 */
#define DRIFT_EVERY_S 15
#define DRIFT_HUNT_MAX_US (2 * 1000000LL)

static drift_t s_drift;
static int64_t s_drift_next_us;      /* when to start looking for the next edge */
static bool    s_drift_hunting;
static int64_t s_drift_hunt_us;      /* when this hunt began, to give it up */
static bool    s_drift_have_prev;
static uint32_t s_drift_prev_chip;
static int64_t s_drift_prev_us;

/*
 * Catch the moment the chip's seconds register increments and note where the
 * board's own clock was. Polling only in the second either side of a sample
 * keeps the I2C traffic down: one burst every fifteen seconds rather than a
 * read every frame for ever.
 *
 * The edge is known only to lie between two polls, so the midpoint of that
 * window is recorded. That centres the error instead of biasing every sample
 * late by the poll interval, which would be invisible in the slope but would
 * put a constant offset on the phase.
 */
static void drift_sample(int64_t now)
{
    if (!s_rtc || !s_synced) return;

    if (!s_drift_hunting) {
        if (s_drift_next_us != 0 && now < s_drift_next_us) return;
        s_drift_hunting = true;
        s_drift_hunt_us = now;
        s_drift_have_prev = false;
    }

    uint32_t chip;
    bool ok = ds3231_read(&chip);
    if (!ok || now - s_drift_hunt_us > DRIFT_HUNT_MAX_US) {
        /* A chip that will not answer, or one that has not ticked in two
           seconds, is not something to keep hammering. Try again next time. */
        s_drift_hunting = false;
        s_drift_next_us = now + (int64_t)DRIFT_EVERY_S * 1000000LL;
        return;
    }

    if (s_drift_have_prev && chip != s_drift_prev_chip) {
        int64_t edge_us = (s_drift_prev_us + now) / 2;
        double board_ms = (double)s_base_secs * 1000.0
                        + (double)(edge_us - s_base_us) / 1000.0;
        double phase = fmod(board_ms - (double)chip * 1000.0, 86400000.0);
        if (phase >  43200000.0) phase -= 86400000.0;
        if (phase < -43200000.0) phase += 86400000.0;

        float xtal = 0.0f;
        bool have_xtal = ds3231_temperature(&xtal);
        drift_add(&s_drift, (float)phase, have_xtal, xtal);

        s_drift_hunting = false;
        s_drift_next_us = now + (int64_t)DRIFT_EVERY_S * 1000000LL;
        return;
    }

    s_drift_have_prev = true;
    s_drift_prev_chip = chip;
    s_drift_prev_us = now;
}

static void draw_rtc(canvas_t *c, int64_t now)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 0, 0, "RTC", PAL_FG);

    if (!s_rtc) {
        canvas_puts(c, 1, 2, "no DS3231 on any bus", PAL_DIM);
        canvas_puts(c, 1, 3, "the clock waits for a Mac", PAL_DIM);
        display_blit();
        return;
    }

    char buf[48];
    i2cbus_id_t bus = ds3231_bus();
    snprintf(buf, sizeof buf, "%s io%d/%d", bus == I2CBUS_MAIN ? "main" : "aux",
             i2cbus_sda(bus), i2cbus_scl(bus));
    canvas_puts(c, 4, 0, buf, PAL_DIM);

    /* How much of the record is on screen, in the corner, as the temperature
       chart does it. */
    int span = drift_span_s(&s_drift);
    if (span >= 3600) snprintf(buf, sizeof buf, "%dh%02dm", span / 3600, (span % 3600) / 60);
    else              snprintf(buf, sizeof buf, "%dm", span / 60);
    canvas_puts(c, c->cols - (int)strlen(buf), 0, buf, PAL_FG);

    /*
     * Row one is the board against the chip; row two is the chip itself. The
     * first word of each is in its trace's colour, which is the only legend
     * a panel this size can afford.
     */
    int n = drift_count(&s_drift);
    canvas_puts(c, 0, 1, "slip", PAL_A0);
    if (n > 0) {
        float ppm;
        int slip = (int)drift_slip_ms(&s_drift);
        if (drift_ppm(&s_drift, &ppm))
            snprintf(buf, sizeof buf, "%+dms %+.1fppm", slip, (double)ppm);
        else
            snprintf(buf, sizeof buf, "%+dms  settling", slip);
        vw_right_text(c, 1, c->cols - 1, buf, PAL_FG);
    } else {
        vw_right_text(c, 1, c->cols - 1, s_synced ? "waiting for a tick"
                                                  : "no clock yet", PAL_DIM);
    }

    canvas_puts(c, 0, 2, "xtal", PAL_A1);
    float xtal = 0.0f;
    int8_t aging = 0;
    bool stopped = false;
    bool have_xtal = ds3231_temperature(&xtal);
    bool have_aging = ds3231_aging(&aging) && ds3231_stopped(&stopped);
    if (have_xtal && have_aging)
        snprintf(buf, sizeof buf, "%.2fC age%+d %s", (double)xtal, aging,
                 stopped ? "OSF" : "ok");
    else if (have_xtal)
        snprintf(buf, sizeof buf, "%.2fC", (double)xtal);
    else
        snprintf(buf, sizeof buf, "unreadable");
    vw_right_text(c, 2, c->cols - 1, buf, stopped ? PAL_A5 : PAL_FG);

    if (n < 2) {
        canvas_puts(c, 0, 4, "  charting the slip...", PAL_DIM);
        display_blit();
        return;
    }
    drift_draw(&s_drift, c, 3);
    display_blit();
}

/* One line of text, centred on the page. */
static void centred(canvas_t *c, int row, const char *s, uint16_t colour)
{
    int col = (c->cols - (int)strlen(s)) / 2;
    canvas_puts(c, col < 0 ? 0 : col, row, s, colour);
}

/*
 * Date above the digits, weather below, both sent from the Mac. Centred so
 * they read as part of the clock rather than as a caption.
 *
 * The weather is one sentence of about forty characters -- conditions,
 * temperature, humidity, wind, and the day's high and low -- which is a third
 * of the big panel's width and half again more than a small one has. On a
 * narrow panel it is broken over the last two rows at a space rather than
 * being clipped: canvas_puts stops at the edge, so the untruncated version of
 * this simply lost the wind and the forecast without saying so.
 */
static void draw_clock_extras(canvas_t *c)
{
    if (s_data.date[0]) centred(c, 1, s_data.date, PAL_FG);
    if (!s_data.weather[0]) return;

    int len = (int)strlen(s_data.weather);
    if (len <= c->cols) { centred(c, c->rows - 2, s_data.weather, PAL_A0); return; }

    /* The last space that leaves a first line fitting the panel. Falling back
       to a hard break keeps a single very long word from vanishing. */
    int cut = 0;
    for (int i = 0; i < len && i <= c->cols; i++)
        if (s_data.weather[i] == ' ') cut = i;
    if (cut == 0) cut = c->cols < len ? c->cols : len;

    char first[40];
    int n = cut < (int)sizeof first - 1 ? cut : (int)sizeof first - 1;
    memcpy(first, s_data.weather, (size_t)n);
    while (n > 0 && first[n - 1] == ' ') n--;      /* the break sits on a space */
    first[n] = '\0';
    centred(c, c->rows - 2, first, PAL_A0);

    const char *rest = s_data.weather + cut;
    while (*rest == ' ') rest++;
    if (*rest) centred(c, c->rows - 1, rest, PAL_A0);
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
    char buf[16];
    timecalc_format_hms(secs, buf);
#if CLOCK_MILLISECONDS
    /*
     * The thousandths, from the same microsecond counter the seconds come
     * from. They are drawn every frame rather than every second, so the page
     * redraws about thirty times a second and the last digit is never
     * actually still -- which is the point of showing it.
     */
    int ms = (int)(((uint64_t)(now - s_base_us) / 1000ULL) % 1000ULL);
    snprintf(buf + 8, sizeof buf - 8, ".%03d", ms);
#else
    if ((int)secs == s_drawn_second) return;
#endif
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
#if HAVE_BUTTONS
    /* Losing the buttons costs navigation, not the display, so carry on. */
    if (buttons_init() != ESP_OK) ESP_LOGW(TAG, "buttons unavailable");
#endif
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    big = true;
    touch = gt911_init() == ESP_OK;
#endif
    /* No finger, no MENU tab: it cannot be pressed, and draw_message paints
       it over the text's first six columns. */
    vw_set_touch(touch);
    /* Which pages this board offers: the data pages need the big panel, and
       the menu and settings need a finger. */
    unsigned available = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        const page_def_t *pd = &page_defs[i];
        if (!(pd->where & (big ? PG_BIG : PG_SMALL))) continue;
        if (pd->needs_touch && !touch) continue;
        available |= PAGE_BIT(i);
    }
    /* Both IMU pages need a sensor, which the page table cannot know about:
       it describes boards, and this is a question about what is plugged into
       one today. A board compiled without either never offers them at all. */
    unsigned imu_pages = PAGE_BIT(PAGE_LEVEL) | PAGE_BIT(PAGE_PARTICLES)
                       | PAGE_BIT(PAGE_TIMER) | PAGE_BIT(PAGE_STOPWATCH);
#if HAVE_IMU
    s_imu = qmi8658_init() == ESP_OK;
    if (!s_imu) available &= ~imu_pages;
#else
    available &= ~imu_pages;
#endif
#if !HAVE_LEVEL
    available &= ~PAGE_BIT(PAGE_LEVEL);
#endif
#if !HAVE_PARTICLES
    available &= ~(PAGE_BIT(PAGE_PARTICLES) | PAGE_BIT(PAGE_TIMER)
                 | PAGE_BIT(PAGE_STOPWATCH));
#endif
    /* Any board may have a DS3231 wired to its I2C bus, so every board asks.
       One without simply does without, the way a board without an IMU does --
       ds3231_init() says which, and everything downstream is gated on s_rtc at
       runtime already. If the chip knows the time, start from it, so the
       display is right before any Mac has said anything. */
    s_rtc = ds3231_init() == ESP_OK;

    /*
     * The clock chip's own page, and the temperature chart, both exist to
     * compare two readings: the board's time against the chip's, and the die
     * against the crystal. With no chip there is nothing to compare -- the
     * RTC page would report an absent chip and the chart would draw one flat
     * trace of a number nobody asked about -- so a board without one is not
     * offered either page rather than being offered an empty one.
     */
    if (!s_rtc) available &= ~(PAGE_BIT(PAGE_RTC) | PAGE_BIT(PAGE_TEMPS));

    uint32_t rtc_secs;
    if (s_rtc && ds3231_read(&rtc_secs)) {
        on_time(rtc_secs);
        s_rtc_pending = false;          /* it came from the chip; no need to write it back */
        ESP_LOGI(TAG, "clock set from the RTC");

        /* The chip has been keeping the calendar too, so the board knows what
           day it is before any Mac speaks. Its own shorter wording, because
           this is not the Mac's sentence. */
        rtc_refresh_date();
    }
    pages_init(&s_pages, available);
    /* A slideshow, on the boards configured for one. Set before the saver is
       applied, because a rotating board turns the saver off. */
    pages_set_rotate(&s_pages, (int64_t)CONFIG_SCREEN_ROTATE_SECS * 1000000LL);
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
    tempsense_init();
    templog_init(&s_templog, TEMPLOG_EVERY_S);
    drift_init(&s_drift, DRIFT_EVERY_S);
    /* Not finding one is ordinary: the board does without, as it does
       without a clock. It says so in the log either way, so a module that
       is plugged in but silent is distinguishable from one that is absent. */
    bme280_init();
#if HAVE_IMU
    if (s_imu) axis_load();      /* the sensor's, so both pages want it */
#endif
#if HAVE_LEVEL
    if (s_imu) zero_load();
#endif
#if HAVE_PARTICLES
    if (s_imu) {
        /* Seeded from the hardware RNG rather than a constant, so the grains
           do not land in the same places every boot. Bluetooth is already
           running, so it is properly seeded. */
        /* esp_random() is the real entropy here; the die's own noise is
           mixed in because it costs nothing and because the barometer that
           used to seed the grains went out with the board it served. */
        uint32_t seed = esp_random() ^ tempsense_entropy();
        s_gravity_px  = GRAVITY_PER_ROW * (float)c->h;
        if (s_gravity_px > GRAVITY_MAX) s_gravity_px = GRAVITY_MAX;
        s_shake_floor = SHAKE_FLOOR_PER_ROW * (float)c->h;
        s_shake_max   = SHAKE_MAX_PER_ROW * (float)c->h;
        particles_init(&s_particles, particles_for(c->w, c->h), c->w, c->h, seed);
        ESP_LOGI(TAG, "sand: %d grains on %dx%d, gravity %.0f, seed 0x%08X",
                 s_particles.n, c->w, c->h, (double)s_gravity_px, (unsigned)seed);
        shaketimer_init(&s_timer, TIMER_DEFAULT_S);
        stopwatch_reset(&s_watch);
        shakedet_init(&s_shake);
    }
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
        if (s_imu && (s_pages.current == PAGE_LEVEL
                   || s_pages.current == PAGE_PARTICLES))
            period_us = 33000;      /* 30 fps while the sensor drives it */
#endif
#if HAVE_PARTICLES
        if (s_imu && s_pages.current == PAGE_PARTICLES)
            period_us = 33000;      /* the liquid wants every frame it can get */
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
            ds3231_date_t d = { s_data.date_year, s_data.date_month,
                                s_data.date_day, s_data.date_wday };
            /* An invalid date leaves the chip's own calendar running rather
               than resetting it, so a sync from a Mac that sent no date does
               not lose the day the chip has been keeping. */
            /*
             * Both clocks move under the measurement here: the chip is being
             * set, and the board's own base was just reset by the sync that
             * asked for it. Every sample already taken was against a
             * different pair of clocks, so the record is started again rather
             * than carried across the discontinuity -- a step of hundreds of
             * milliseconds in the middle of a fit would read as an enormous
             * and entirely fictional ppm.
             */
            drift_init(&s_drift, DRIFT_EVERY_S);
            s_drift_hunting = false;
            s_drift_next_us = now + (int64_t)DRIFT_EVERY_S * 1000000LL;

            if (ds3231_write(now_secs(now), ds3231_date_valid(&d) ? &d : NULL))
                ESP_LOGI(TAG, "RTC set from the Mac%s",
                         ds3231_date_valid(&d) ? ", with the date" : "");
        }
        /*
         * A board left running crosses midnight without anyone touching it,
         * and the date it read at boot is then a day stale. Our own clock
         * wrapping is the cheapest signal that the day has turned: ask the
         * chip, which has already rolled over on its own battery.
         */
        if (s_rtc && s_synced) {
            uint32_t day_secs = now_secs(now);
            if (day_secs < s_last_day_secs) rtc_refresh_date();
            s_last_day_secs = day_secs;
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
            /* A wrap can be missed -- a board asleep, a clock corrected
               backwards over midnight -- so the hourly check carries the
               date too, and a day can never be stale for longer than that. */
            rtc_refresh_date();
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
                if (s_menu_held == target && now - s_menu_held_us < MENU_DOUBLE_US) {
                    /* The second tap. The page is not opened at all: whoever
                       double-tapped was setting the tile, not going there. */
                    toggle_cycle(target);
                    s_menu_held = PAGE_COUNT;
                    s_pages.last_activity_us = now;
                    ESP_LOGI(TAG, "tap at %d,%d -> menu -> toggled page %d",
                             tx, ty, (int)target);
                } else {
                    s_menu_held = target;
                    s_menu_held_us = now;
                    s_pages.last_activity_us = now;
                    ESP_LOGI(TAG, "tap at %d,%d -> menu -> holding page %d",
                             tx, ty, (int)target);
                }
                s_drawn_page = PAGE_COUNT;
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

        /* The double-tap window has closed with no second tap, so the first
           one meant what it said. */
        if (s_menu_held != PAGE_COUNT && now - s_menu_held_us >= MENU_DOUBLE_US) {
            page_t target = s_menu_held;
            s_menu_held = PAGE_COUNT;
            if (s_pages.current == PAGE_MENU) {
                pages_show(&s_pages, target, now);
                ESP_LOGI(TAG, "menu -> page %d", (int)target);
            }
            s_drawn_page = PAGE_COUNT;
        }
#if HAVE_BUTTONS
        button_t press = buttons_pressed();
#if HAVE_PARTICLES
        /* The timer page borrows the button: holding it dials the duration
           rather than turning the page, and a tap then accepts what has been
           dialled. Everywhere else the button means what it always means. */
        if (press == BUTTON_PREV && s_pages.current == PAGE_STOPWATCH) {
            stopwatch_reset(&s_watch);
            ESP_LOGI(TAG, "stopwatch cleared");
            press = BUTTON_NONE;
        }
        if (press != BUTTON_NONE && s_pages.current == PAGE_TIMER) {
            if (shaketimer_setting(&s_timer)) {
                if (press == BUTTON_NEXT) {
                    shaketimer_accept(&s_timer);
                    ESP_LOGI(TAG, "timer set to %d s", s_timer.duration_s);
                }
                press = BUTTON_NONE;
            } else if (press == BUTTON_PREV) {
                shaketimer_begin_set(&s_timer);
                ESP_LOGI(TAG, "dialling the timer");
                press = BUTTON_NONE;
            }
        }
#endif
        if (press != BUTTON_NONE) {
            if (s_saver) {
                /* As with a tap, the first press only wakes: changing the page
                   as well would lose whatever was on screen before the saver. */
                s_pages.last_activity_us = now;
                ESP_LOGI(TAG, "button -> wake");
            } else if (press == BUTTON_PREV) {
                page_t p = pages_back(&s_pages, now);
                ESP_LOGI(TAG, "button -> back to page %d", (int)p);
            } else {
                page_t p = pages_advance(&s_pages, now);
                ESP_LOGI(TAG, "button -> page %d", (int)p);
            }
            s_auto_jumped = false;          /* a press means a person is choosing */
        }
#endif

        templog_sample(now);
        drift_sample(now);

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
        /* The level and the sand are things you are using, not things left
           on display, so the saver must not take them away underneath you --
           and tilting a board sends it nothing, which is exactly what looks
           like idling. Neither needs protecting from burn-in: both move. */
        if (s_pages.current == PAGE_LEVEL || s_pages.current == PAGE_PARTICLES)
            s_pages.last_activity_us = now;
#endif
#if HAVE_PARTICLES
        /* The liquid is the same case: something you are watching rather than
           something left on display, and it needs no protecting from burn-in
           because every grain is moving. */
        if (s_pages.current == PAGE_PARTICLES) s_pages.last_activity_us = now;
        /* A screensaver that ate a running countdown would leave you with a
           clock instead of an answer. */
        if (s_pages.current == PAGE_TIMER) s_pages.last_activity_us = now;
        if (s_pages.current == PAGE_STOPWATCH) s_pages.last_activity_us = now;
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

        /* Once idle, move on. On a board that rotates this is the normal
           way it is read -- you watch it rather than drive it -- and it
           replaces the screensaver rather than running alongside one. */
        if (pages_tick(&s_pages, now, cycle_skip())) {
            s_drawn_page = PAGE_COUNT;      /* force a full redraw */
            s_drawn_second = -1;
            /* Which pages a board actually rotates through is decided by
               three things at once -- the panel size, what is plugged in, and
               what has been struck off the menu -- so it is worth saying
               rather than inferring from a heartbeat. */
            ESP_LOGI(TAG, "rotated to %s", page_defs[s_pages.current].name);
        }

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
                case PAGE_MENU:     views_menu(c, &s_pages, s_cycle_off, s_menu_held); display_blit(); break;
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
        if (s_pages.current == PAGE_RTC) draw_rtc(c, now);
        if (s_pages.current == PAGE_TEMPS) {
            templog_draw(&s_templog, c);
            display_blit();
        }
#if HAVE_LEVEL
        if (s_pages.current == PAGE_LEVEL && s_imu) draw_level(c);
#endif
#if HAVE_PARTICLES
        if (s_pages.current == PAGE_PARTICLES && s_imu) draw_particles(c, now);
        if (s_pages.current == PAGE_TIMER && s_imu) draw_timer(c, now);
        if (s_pages.current == PAGE_STOPWATCH && s_imu) draw_stopwatch(c, now);
#endif
        /* Pages with ages on them redraw on their own so the ages keep counting. */
        if (pd->refresh_us > 0 && now - s_page_drawn_us > pd->refresh_us)
            s_drawn_page = PAGE_COUNT;

        /* USB-Serial-JTAG drops output when no host is attached, so the boot
           log is often missed. A heartbeat makes liveness observable. */
        if (now - last_beat > 30 * 1000000LL) {
            last_beat = now;
            float die = 0.0f;
            bool have_die = tempsense_read(&die);
            float air = 0.0f, hpa = 0.0f, rh = 0.0f;
            bool have_air = bme280_read(&air, &hpa, &rh);
            ESP_LOGI(TAG, "alive, page %d, clock %s, rtc %s, die %.1f C",
                     (int)s_pages.current, s_synced ? "synced" : "unset",
                     s_rtc ? "present" : "absent", have_die ? (double)die : -1.0);
            /* What the RTC page is charting, for a board being watched over
               the wire rather than looked at. The ppm needs a few minutes of
               samples before it means anything and says so until then. */
            if (s_rtc && drift_count(&s_drift) > 0) {
                float ppm;
                int n = drift_count(&s_drift);
                if (drift_ppm(&s_drift, &ppm))
                    ESP_LOGI(TAG, "board vs chip: %+d ms over %d min, %+.1f ppm",
                             (int)drift_slip_ms(&s_drift),
                             drift_span_s(&s_drift) / 60, (double)ppm);
                else
                    ESP_LOGI(TAG, "board vs chip: %+d ms, %d samples, still settling",
                             (int)drift_slip_ms(&s_drift), n);
            }
            if (have_air)
                ESP_LOGI(TAG, "room %.1f C, %.0f%% RH, %.1f hPa",
                         (double)air, (double)rh, (double)hpa);
        }
    }
}
