#include "display.h"
#include "drift.h"
#include "ble_uart.h"
#include "bme280.h"
#include "buttons.h"
#include "touch.h"
#include "level.h"
#include "particles.h"
#include "qmi8658.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_random.h"
#include <math.h>
#include "particles.h"
#include "pagedefs.h"
#include "pages.h"
#include "view_common.h"
#include "ds3231.h"
#if CONFIG_SCREEN_ENV_ONLY
#include "aht21.h"
#include "ens160.h"
#include "envflash.h"
#include "envpage.h"
#include "envstore.h"
#endif
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
#include "axp2101.h"
#include "levelbig.h"
#include "i2cbus.h"
#endif
#if CONFIG_SCREEN_HAVE_CAMERA
#include "camera.h"
#include "sdcard.h"
#include "img_converters.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#endif
#if CONFIG_SCREEN_BOARD_TOUCH_LCD_147
/* envo logs a reading to the microSD every 30 s as well as to flash. */
#include "sdcard.h"
#endif
#include "settings.h"
#include "shaketimer.h"
#include "pip.h"
#include "templog.h"
#include "tempsense.h"
#include "textwrap.h"
#include "timecalc.h"
#include "usagedata.h"

#include "palette.h"
#include "views.h"

#include "esp_log.h"
#include "nvs.h"
#include "esp_random.h"
#include <math.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "main";

#define TICK_MS 50            /* also the touch and button poll interval */

/*
 * Thousandths on the clock. They cost a redraw every frame instead of every
 * second, which is most of what this board does while the clock is showing,
 * so it is a choice rather than a given -- and a per-board one, since the
 * boards are not doing the same job. See SCREEN_CLOCK_MS in Kconfig.
 */
#ifdef CONFIG_SCREEN_CLOCK_MS
#define CLOCK_MILLISECONDS 1
#else
#define CLOCK_MILLISECONDS 0
#endif

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
 * Boards with a finger. Two so far and nothing in common but the job: a GT911
 * on the CrowPanel and a CST816D on envo. Having touch is not the same as
 * having the big panel, which is why this is its own test -- the two were the
 * same board until envo arrived, and the code said "CrowPanel" where it meant
 * "has touch".
 */
#if defined(CONFIG_SCREEN_BOARD_CROWPANEL_7) || defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
#define HAVE_TOUCH 1
#else
#define HAVE_TOUCH 0
#endif

/* Touch that pages by halves rather than through a menu. Nothing uses it now
   that envo has gone, but it is two lines and the next touch board may want
   it: the CrowPanel is the only one here with a finger, and it has the room
   for a menu. */
#define TOUCH_BY_HALVES 0


/*
 * Anything that needs the IMU. Two boards have one: the Feather, where a
 * QMI8658 hangs off the STEMMA QT port, and wave, where the same chip is
 * soldered to the board at GPIO48/47. lilly and the CrowPanel have no sensor
 * and are compiled with none of this.
 *
 * An environment logger has a sensor and no use for it: the sand, the level
 * and the shake timer are not what that board is for, and the driver they
 * need goes with them. CMakeLists drops the same files, so a reference left
 * behind here shows up as a link error rather than as dead code.
 */
/* Two boards read the IMU. wave is back -- a QMI8658 soldered at GPIO48/47 --
   and she is a games board: the sand and the spirit level, as she was before
   she left. envio has her QMI8658 too, but only for the level -- the
   room-and-weather pages read the gas sensor on her I2C bus, not the
   accelerometer, and she is not a games board for Pip or the sand. lilly and
   the CrowPanel have no sensor; the IMU driver and Pip stay in the tree,
   excluded by CMakeLists on the boards that do not want them. */
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B) \
 || defined(CONFIG_SCREEN_BOARD_WAVESHARE_147B)
#define HAVE_IMU 1
#else
#define HAVE_IMU 0
#endif
#define HAVE_PIP 0

/* What each board does with it. Both pour the grains; only the Feather also
   reads the sensor as an instrument. The axis mapping below is shared,
   because it describes the sensor rather than either page -- which is exactly
   why the sand has to negate one component of it: a bubble floats against
   gravity and grains fall with it. */
/* Both sensor boards read the level; it is the same instrument either way. A
   Pip board keeps its IMU for the face and does not offer the level or sand.
   envio now does too, at her own touch page (PAGE_BUBBLE) rather than
   PAGE_LEVEL -- the Feather's spirit-level game replaced the sand-free
   bubblelevel.c there. She still has no sand: CMakeLists keeps
   particles.c excluded for her. */
#define HAVE_LEVEL (HAVE_IMU && !HAVE_PIP)

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
/* The grains pour on either board that has the sensor -- but not on a Pip
   board, whose IMU is Pip's, and not on envio, whose CMakeLists drops
   particles.c along with level.c. */
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
#define HAVE_PARTICLES 0
#else
#define HAVE_PARTICLES (HAVE_IMU && !HAVE_PIP)
#endif

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

/*
 * The whole game, shared by every board that has it: reads the sensor,
 * filters it, works out how far off level the panel is, and runs the
 * arm/hold/score clock against that. Returns false (and touches nothing)
 * when the sensor did not answer this frame, exactly as draw_level used to
 * bail before ever reaching level_draw.
 *
 * `*tx_out`/`*ty_out` come back ABSOLUTE -- never adjusted for "!zero". The
 * scoring itself still honours a zero, on whichever board can set one (see
 * below): a board zeroed by hand should still have to hold *that* level to
 * keep the clock running, exactly as before this was factored out of
 * draw_level. A caller that wants the zeroed reading for its own display
 * applies the same subtraction itself, as draw_level does just below. envio's
 * draw_levelbig does not, which is what makes "!zero" inert for her -- and
 * the command handler further down never lets her reach zero_command at all,
 * so the point is moot there, but this keeps the two boards' math identical
 * either way.
 */
static bool level_step(float *tx_out, float *ty_out)
{
    qmi8658_sample_t sample;
    if (qmi8658_read(&sample) != ESP_OK) return false;

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

    /* Scored against the zeroed reading when there is one -- absolute tx/ty
       is what callers get back. */
    float zx = tx, zy = ty;
    if (s_zeroed) { zx -= s_zero_x; zy -= s_zero_y; }

    /* The clock. It runs while the board is true and resets the moment it is
       not, which is the whole game. */
    if (!level_is_true(zx, zy)) s_armed = true;

    if (level_is_true(zx, zy) && s_armed) {
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

    *tx_out = tx;
    *ty_out = ty;
    return true;
}

static void draw_level(canvas_t *c)
{
    float tx, ty;
    if (!level_step(&tx, &ty)) return;
    if (s_zeroed) { tx -= s_zero_x; ty -= s_zero_y; }

    /* One letter, bottom right: z means the angles are relative to a surface
       taken as true with "!zero", nothing means they are absolute. */
    level_draw(c, tx, ty, s_hold_s, s_last_hold_s, s_prev_hold_s,
               s_best_hold_s, s_armed);
    display_blit();
}

#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
/* envio's own big rendering of the same game, in levelbig.c: absolute tx/ty
   (a "!zero" can never reach her -- see the command gate below), drawn large
   for her tall panel instead of the Feather's small one. */
static void draw_levelbig(canvas_t *c)
{
    float tx, ty;
    if (!level_step(&tx, &ty)) return;
    levelbig_draw(c, tx, ty, s_hold_s, s_last_hold_s, s_prev_hold_s,
                  s_best_hold_s, s_armed);
    display_blit();
}
#endif
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

#if HAVE_PIP
/*
 * Pip's page: read the IMU, turn it into gravity and a shake, and let the face
 * react. gravity_from is the same mapping the sand and the level use, so a
 * "!flip" that gets Pip's eyes looking the right way is the board's one axis
 * calibration, kept in NVS. The face itself is in pip.c and host-tested.
 */
static pip_t s_pip;
static shakedet_t s_pip_shake;
static int64_t s_pip_last_us;

static void draw_pip(canvas_t *c, int64_t now)
{
    float dt = s_pip_last_us ? (float)(now - s_pip_last_us) / 1000000.0f : 0.05f;
    s_pip_last_us = now;
    if (dt > 0.2f) dt = 0.2f;

    float gx = 0.0f, gy = 0.0f;
    bool shaken = false;
    qmi8658_sample_t sample;
    if (s_imu && qmi8658_read(&sample) == ESP_OK) {
        gravity_from(&sample, &gx, &gy);
        shaken = shakedet_update(&s_pip_shake, sample.ax, sample.ay, sample.az,
                                 dt, now);
    }
    pip_update(&s_pip, gx, gy, shaken, dt);
    pip_draw(c, &s_pip);
    display_blit();
}
#endif /* HAVE_PIP */

#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
/* System/status page: battery, power source, chip temp, free RAM, uptime and
   clock state. envio has no microphone (no codec on the 3.5B), so nothing
   audio-related belongs here. */
static void draw_system(canvas_t *c)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, "System", PAL_FG);

    char buf[48];
    int row = 2;

    /* The gauge changes on a multi-second scale, but this page redraws every
       tick to keep the uptime clock moving -- so the I2C read itself is
       throttled to about once a second and the cached sample reused for the
       ticks in between, rather than hitting the PMIC 20x/s for numbers that
       have not moved. */
    static axp2101_batt_t s_batt_cache;
    static bool s_batt_valid;
    static int64_t s_batt_last_us;
    int64_t now_us = esp_timer_get_time();
    if (!s_batt_valid || now_us - s_batt_last_us > 1000000) {
        s_batt_valid = axp2101_battery(&s_batt_cache);
        s_batt_last_us = now_us;
    }
    axp2101_batt_t batt = s_batt_cache;
    bool have_batt = s_batt_valid;
    if (have_batt) {
        const char *state = batt.charge == AXP_CHG_CHARGING ? "charging"
                           : batt.charge == AXP_CHG_DISCHARGING ? "on battery"
                           : "idle";
        uint16_t state_colour = batt.charge == AXP_CHG_CHARGING ? PAL_A1 : PAL_FG;
        if (batt.percent >= 0)
            snprintf(buf, sizeof buf, "Battery  %d%%  %s", batt.percent, state);
        else
            snprintf(buf, sizeof buf, "Battery  --  %s", state);
        canvas_puts(c, 0, row++, buf, state_colour);

        snprintf(buf, sizeof buf, "         %.2f V", batt.millivolts / 1000.0);
        canvas_puts(c, 0, row++, buf, PAL_FG);

        snprintf(buf, sizeof buf, "Power    %s", batt.vbus ? "USB in" : "on battery");
        canvas_puts(c, 0, row++, buf, PAL_FG);
    } else {
        canvas_puts(c, 0, row++, "Battery  no PMIC answer", PAL_DIM);
    }

    float die = 0.0f;
    if (tempsense_read(&die)) {
        snprintf(buf, sizeof buf, "Chip     %.1f C", (double)die);
        canvas_puts(c, 0, row++, buf, PAL_FG);
    }

    snprintf(buf, sizeof buf, "Free RAM %u KB", (unsigned)(esp_get_free_heap_size() / 1024));
    canvas_puts(c, 0, row++, buf, PAL_FG);

    int64_t up_s = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof buf, "Uptime   %lldh %02lldm",
             (long long)(up_s / 3600), (long long)((up_s / 60) % 60));
    canvas_puts(c, 0, row++, buf, PAL_FG);

    snprintf(buf, sizeof buf, "Clock    %s", s_synced ? "RTC ok" : "no time");
    canvas_puts(c, 0, row++, buf, PAL_FG);

    display_blit();
}
#endif /* CONFIG_SCREEN_BOARD_TOUCH_LCD_35B */

#if CONFIG_SCREEN_HAVE_CAMERA
/* "2026-09-19 18:34" from the DS3231, or a plain fallback if there is no
   clock to ask -- a photo taken before the RTC has ever been set should
   still get a JPEG, just without a trustworthy stamp on it. */
static void camera_timestamp(char *out, size_t n)
{
    ds3231_date_t date;
    uint32_t secs;
    if (s_rtc && ds3231_read_date(&date) && date.year >= 2000
        && ds3231_read(&secs)) {
        int hh = (int)(secs / 3600) % 24;
        int mm = (int)(secs / 60) % 60;
        snprintf(out, n, "%04d-%02d-%02d %02d:%02d",
                 date.year, date.month, date.day, hh, mm);
    } else {
        snprintf(out, n, "no clock set");
    }
}

/* The message shown over the viewfinder right after a shot -- "saved
   envio/IMG_...jpg" or why it failed -- for a couple of seconds before the
   live preview takes the screen back. */
static char s_shot_msg[80] = "";
static int64_t s_shot_msg_until = 0;

/* Tap on PAGE_CAMERA: grab a raw RGB565 frame (not sensor-JPEG, so the
   timestamp can be drawn onto the pixels first), stamp it, encode with
   fmt2jpg, and write it to the card.
   The "saved ..." deadline below is set from a fresh esp_timer_get_time()
   once the work (deinit/reinit, settling frames, a software JPEG encode,
   the SD write) is actually done -- using the tap's timestamp from before
   all that would put the deadline in the past and the message would never
   show. */
static void camera_shutter(void)
{
    camera_fb_t *fb = NULL;
    esp_err_t err = camera_capture_rgb(&fb);
    if (err != ESP_OK) {
        /* camera_capture_rgb already falls back to preview itself on this
           path (fall_back_to_preview, camera.c) -- a second
           camera_resume_preview() here would just be a redundant reinit. */
        ESP_LOGE(TAG, "camera_shutter: capture failed: %s", esp_err_to_name(err));
        snprintf(s_shot_msg, sizeof s_shot_msg, "capture failed");
        s_shot_msg_until = esp_timer_get_time() + 2000000;
        return;
    }

    /* Wrap the fb's own buffer as a canvas so the timestamp is baked into
       the pixels before encoding, not drawn over the JPEG afterwards. */
    canvas_t tmp;
    canvas_init(&tmp, (uint16_t *)fb->buf, (int)fb->width, (int)fb->height, 1);

    char stamp[32];
    camera_timestamp(stamp, sizeof stamp);
    int ty = tmp.h - tmp.cell_h - 4;
    /* White on a faux black outline (four offset copies behind it) so it
       reads whether the frame behind it is bright or dark.
       CANVAS_FG/CANVAS_BG (0xFFFF/0x0000) are swap-symmetric -- bswap16 of
       either is itself -- so drawing them straight into this big-endian
       sensor buffer (see camera_preview's swap in camera.c) happens to come
       out right without any conversion. That is a coincidence of these two
       particular values, not a property of this code: a future coloured
       stamp drawn here would need the same byte-swap camera_preview does
       before this buffer reaches the screen, or fmt2jpg before it reaches
       the JPEG (fmt2jpg itself expects the sensor's own big-endian order,
       so a colour stamped in native/panel order would come out wrong in the
       saved photo even though CANVAS_FG/BG do not). */
    canvas_puts_px(&tmp, 5, ty,     stamp, CANVAS_BG);
    canvas_puts_px(&tmp, 7, ty,     stamp, CANVAS_BG);
    canvas_puts_px(&tmp, 6, ty - 1, stamp, CANVAS_BG);
    canvas_puts_px(&tmp, 6, ty + 1, stamp, CANVAS_BG);
    canvas_puts_px(&tmp, 6, ty,     stamp, CANVAS_FG);

    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    /* fmt2jpg's quality is jpge's 1-100 (higher is better) -- unlike
       camera_config_t.jpeg_quality, which is the sensor's inverted 0-63.
       Reusing 12 from that other scale would give a badly blocky photo. */
    bool ok = fmt2jpg(fb->buf, fb->len, (uint16_t)fb->width, (uint16_t)fb->height,
                      PIXFORMAT_RGB565, 80, &jpg, &jpg_len);
    if (ok) {
        char name[64];
        sd_photo_name(name, sizeof name);
        esp_err_t werr = sd_write(name, jpg, jpg_len);
        free(jpg);   /* fmt2jpg mallocs; ours to free either way */
        if (werr == ESP_OK) {
            snprintf(s_shot_msg, sizeof s_shot_msg, "saved %s", name);
            ESP_LOGI(TAG, "camera_shutter: wrote %s (%u bytes)",
                     name, (unsigned)jpg_len);
        } else {
            ESP_LOGE(TAG, "camera_shutter: sd_write failed: %s",
                     esp_err_to_name(werr));
            snprintf(s_shot_msg, sizeof s_shot_msg, "save failed");
        }
    } else {
        ESP_LOGE(TAG, "camera_shutter: fmt2jpg failed");
        snprintf(s_shot_msg, sizeof s_shot_msg, "encode failed");
    }
    s_shot_msg_until = esp_timer_get_time() + 2000000;

    esp_camera_fb_return(fb);
    camera_resume_preview();
}

/* The camera page: a live viewfinder while the page is held, "tap to
   capture" underneath it, and briefly a "saved ..." confirmation over the
   preview right after a shot. */
/* The on-screen shutter: a labelled rectangle near the bottom, so capture is a
   deliberate button press rather than a tap anywhere (which fired while aiming). */
static void shot_button_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    *w = 140;
    *h = 52;
    *x = (c->w - *w) / 2;
    *y = c->h - *h - 10;
}

static bool in_shot_button(const canvas_t *c, int px, int py)
{
    int x, y, w, h;
    shot_button_rect(c, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Resolution picker, top-left: VGA -> SVGA -> UXGA -> VGA. */
static void res_button_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    (void)c;
    *x = 4; *y = 4; *w = 96; *h = 26;
}

static bool in_res_button(const canvas_t *c, int px, int py)
{
    int x, y, w, h;
    res_button_rect(c, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

static const char *res_label(framesize_t sz)
{
    switch (sz) {
    case FRAMESIZE_VGA:  return "RES VGA";
    case FRAMESIZE_UXGA: return "RES UXGA";
    default:             return "RES SVGA";   /* FRAMESIZE_SVGA, the default */
    }
}

static void camera_cycle_resolution(void)
{
    switch (camera_get_capture_size()) {
    case FRAMESIZE_VGA:  camera_set_capture_size(FRAMESIZE_SVGA); break;
    case FRAMESIZE_SVGA: camera_set_capture_size(FRAMESIZE_UXGA); break;
    default:              camera_set_capture_size(FRAMESIZE_VGA);  break;
    }
}


static void draw_camera(canvas_t *c)
{
    canvas_clear(c);
    bool has_frame = camera_preview(c);

    int64_t now = esp_timer_get_time();

    if (now < s_shot_msg_until)
        canvas_puts_px(c, 6, 34, s_shot_msg, PAL_A1);
    else if (!has_frame)
        canvas_puts_px(c, 6, 34, "camera warming up", PAL_FG);

    int rx, ry, rw, rh;
    res_button_rect(c, &rx, &ry, &rw, &rh);
    canvas_fill_rect(c, rx, ry, rw, rh, PAL_A0);
    canvas_puts_px(c, rx + 4, ry + (rh - c->cell_h) / 2, res_label(camera_get_capture_size()), PAL_BG);

    int bx, by, bw, bh;
    shot_button_rect(c, &bx, &by, &bw, &bh);
    canvas_fill_rect(c, bx, by, bw, bh, PAL_A1);
    int tw = 4 * c->cell_w;   /* "SHOT" */
    canvas_puts_px(c, bx + (bw - tw) / 2, by + (bh - c->cell_h) / 2, "SHOT", PAL_BG);
    display_blit();
}

/* PAGE_GALLERY: browse the JPEGs camera_shutter saved to /sdcard/envio.
   Filenames only (no path), newest first. Scanned once on entering the
   page (see the s_prev_page tracking below, alongside the camera's own),
   not on every draw -- opendir/readdir on every frame would be wasteful
   and there is no need to notice a photo taken by another session. */
#define GALLERY_MAX 64
static char s_gallery_names[GALLERY_MAX][40];
static int s_gallery_count = 0;
static int s_gallery_idx = 0;

/* Descending by name: IMG_<timestamp>.jpg sorts newest first among the
   normal, clock-set names. The "IMG_boot_<seq>" fallback (sd_photo_name,
   sdcard.c) sorts ahead of all of those ('b' > '2') since it is only ever
   used before the clock has been set -- an ordering quirk worth knowing
   about, not a bug worth working around here. */
static int gallery_name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

static void gallery_scan(void)
{
    s_gallery_count = 0;
    DIR *d = opendir("/sdcard/envio");
    if (!d) return;   /* no card, or no envio/ yet -- draw_gallery says "no photos yet" */

    struct dirent *e;
    while (s_gallery_count < GALLERY_MAX && (e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        /* FATFS_LFN may hand back an 8.3 upper-case alias if long names are
           off, so match the extension case-insensitively. Not filtering on
           d_type: the FAT VFS does not reliably fill it in. */
        if (len < 4 || strcasecmp(e->d_name + len - 4, ".jpg") != 0) continue;
        strncpy(s_gallery_names[s_gallery_count], e->d_name,
                sizeof s_gallery_names[0] - 1);
        s_gallery_names[s_gallery_count][sizeof s_gallery_names[0] - 1] = '\0';
        s_gallery_count++;
    }
    closedir(d);

    qsort(s_gallery_names, s_gallery_count, sizeof s_gallery_names[0],
          gallery_name_cmp);
}

/* Reads the whole file into a PSRAM buffer, decodes at the largest scale
   that still fits the panel (leaving room for the caption), blits it
   centred, and frees both buffers. Any failure along the way draws a
   message instead of the photo -- this must never crash the page. */
static void gallery_draw_photo(canvas_t *c, const char *name)
{
    char path[320];
    snprintf(path, sizeof path, "/sdcard/envio/%s", name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        char msg[80];
        snprintf(msg, sizeof msg, "can't read %s", name);
        canvas_puts_px(c, 6, 2, msg, PAL_FG);
        return;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) {
        fclose(f);
        char msg[80];
        snprintf(msg, sizeof msg, "can't read %s", name);
        canvas_puts_px(c, 6, 2, msg, PAL_FG);
        return;
    }

    uint8_t *jbuf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM);
    if (!jbuf) {
        fclose(f);
        canvas_puts_px(c, 6, 2, "out of memory", PAL_FG);
        return;
    }
    size_t rd = fread(jbuf, 1, (size_t)fsize, f);
    fclose(f);
    if (rd != (size_t)fsize) {
        free(jbuf);
        char msg[80];
        snprintf(msg, sizeof msg, "can't read %s", name);
        canvas_puts_px(c, 6, 2, msg, PAL_FG);
        return;
    }

    /* Native dimensions first, undecoded, so the scale below is picked from
       the real photo (HVGA from camera_capture_rgb today, but this does not
       assume that) rather than a size hardcoded for one capture config. */
    esp_jpeg_image_cfg_t info_cfg = {
        .indata = jbuf,
        .indata_size = (uint32_t)fsize,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK || info.width == 0) {
        free(jbuf);
        char msg[80];
        snprintf(msg, sizeof msg, "can't decode %s", name);
        canvas_puts_px(c, 6, 2, msg, PAL_FG);
        return;
    }

    static const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8
    };
    static const int divs[] = { 1, 2, 4, 8 };
    int max_w = c->w - 8;
    int max_h = c->h - c->cell_h - 16;   /* leaves room for the caption row */
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_1_8;
    int div = 8;
    for (size_t i = 0; i < sizeof divs / sizeof divs[0]; i++) {
        if ((int)info.width / divs[i] <= max_w && (int)info.height / divs[i] <= max_h) {
            scale = scales[i];
            div = divs[i];
            break;
        }
    }

    int out_w = (int)info.width / div;
    int out_h = (int)info.height / div;
    size_t out_len = (size_t)out_w * (size_t)out_h * 2;
    uint16_t *pixels = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
    if (!pixels) {
        free(jbuf);
        canvas_puts_px(c, 6, 2, "out of memory", PAL_FG);
        return;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = jbuf,
        .indata_size = (uint32_t)fsize,
        .outbuf = (uint8_t *)pixels,
        .outbuf_size = (uint32_t)out_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t out = {0};
    esp_err_t derr = esp_jpeg_decode(&cfg, &out);
    free(jbuf);
    if (derr != ESP_OK) {
        free(pixels);
        char msg[80];
        snprintf(msg, sizeof msg, "can't decode %s", name);
        canvas_puts_px(c, 6, 2, msg, PAL_FG);
        return;
    }

    /* Unlike camera_preview's raw sensor frame (big-endian on the wire, so
       it needs the bswap16 loop there), the JPEG decoder above was asked
       for swap_color_bytes=0 -- its own default in this same component's
       fmt2bmp/jpg2rgb565 -- which by inspection already lands in the
       panel's native order. No swap here; if a photo comes out colour-
       swapped on the actual hardware, flip this flag to 1 (or add the same
       __builtin_bswap16 loop camera_preview uses) rather than guessing
       again. */
    canvas_blit(c, pixels, (int)out.width, (int)out.height,
                (c->w - (int)out.width) / 2, (c->h - c->cell_h - 8 - (int)out.height) / 2);
    free(pixels);
}

/* DEL, bottom-left -- small enough to stay clear of the caption row and the
   photo above it. */
static void gallery_del_button_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    *w = 60; *h = 26;
    *x = 4; *y = c->h - c->cell_h - *h - 6;
}

static bool in_gallery_del_button(const canvas_t *c, int px, int py)
{
    int x, y, w, h;
    gallery_del_button_rect(c, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Prev/next arrows, one on each side, vertically centred, for stepping
   through the photos. */
static void gallery_next_button_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    *w = 52; *h = 72;
    *x = c->w - *w - 4; *y = (c->h - *h) / 2;
}

static void gallery_prev_button_rect(const canvas_t *c, int *x, int *y, int *w, int *h)
{
    *w = 52; *h = 72;
    *x = 4; *y = (c->h - *h) / 2;
}

static bool in_rect(const canvas_t *c, int px, int py,
                    void (*rect)(const canvas_t *, int *, int *, int *, int *))
{
    int x, y, w, h;
    rect(c, &x, &y, &w, &h);
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* Removes the file under s_gallery_idx from the card and drops it from the
   in-memory list (a shift, not a re-scan -- cheap, and gallery_scan's own
   newest-first order is already exactly what the shift preserves). */
static void gallery_delete_current(void)
{
    if (s_gallery_count == 0) return;
    char path[320];
    snprintf(path, sizeof path, "/sdcard/envio/%s", s_gallery_names[s_gallery_idx]);
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "gallery_delete_current: remove %s failed: %s",
                 path, strerror(errno));
        return;
    }
    ESP_LOGI(TAG, "gallery_delete_current: removed %s", path);
    for (int i = s_gallery_idx; i < s_gallery_count - 1; i++)
        strncpy(s_gallery_names[i], s_gallery_names[i + 1], sizeof s_gallery_names[0]);
    s_gallery_count--;
    if (s_gallery_idx >= s_gallery_count) s_gallery_idx = 0;
}

static void draw_gallery(canvas_t *c)
{
    canvas_clear(c);

    if (s_gallery_count == 0) {
        canvas_puts(c, 0, 0, "no photos yet", PAL_FG);
        display_blit();
        return;
    }
    if (s_gallery_idx >= s_gallery_count) s_gallery_idx = 0;

    gallery_draw_photo(c, s_gallery_names[s_gallery_idx]);

    char cap[96];
    uint64_t free_b;
    if (sd_free_bytes(&free_b)) {
        double free_gb = (double)free_b / (1024.0 * 1024.0 * 1024.0);
        snprintf(cap, sizeof cap, "%d/%d  %.1fGB free",
                 s_gallery_idx + 1, s_gallery_count, free_gb);
    } else {
        snprintf(cap, sizeof cap, "%d/%d", s_gallery_idx + 1, s_gallery_count);
    }
    canvas_puts(c, 0, c->rows - 1, cap, PAL_FG);

    int dx, dy, dw, dh;
    gallery_del_button_rect(c, &dx, &dy, &dw, &dh);
    canvas_fill_rect(c, dx, dy, dw, dh, 0xF800);   /* red: delete is destructive */
    canvas_puts_px(c, dx + (dw - 3 * c->cell_w) / 2, dy + (dh - c->cell_h) / 2, "DEL", PAL_FG);

    int px, py, pw, ph;
    gallery_prev_button_rect(c, &px, &py, &pw, &ph);
    canvas_fill_rect(c, px, py, pw, ph, PAL_A0);
    canvas_puts_px(c, px + (pw - c->cell_w) / 2, py + (ph - c->cell_h) / 2, "<", PAL_BG);
    int nx, ny, nw, nh;
    gallery_next_button_rect(c, &nx, &ny, &nw, &nh);
    canvas_fill_rect(c, nx, ny, nw, nh, PAL_A0);
    canvas_puts_px(c, nx + (nw - c->cell_w) / 2, ny + (nh - c->cell_h) / 2, ">", PAL_BG);

    display_blit();
}
#endif /* CONFIG_SCREEN_HAVE_CAMERA */

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
    /* envio comes home to Pip -- her whole reason for a screen. */
    if (s_pages.available & PAGE_BIT(PAGE_PIP)) return PAGE_PIP;
    if (s_pages.available & PAGE_BIT(PAGE_PARTICLES)) return PAGE_PARTICLES;
    /* envio is a camera app: the viewfinder is home, checked ahead of the
       level so a board with both (hers) comes home to the camera. */
    if (s_pages.available & PAGE_BIT(PAGE_CAMERA)) return PAGE_CAMERA;
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
static page_mask_t s_cycle_off;
#define MENU_DOUBLE_US 400000
static page_t s_menu_held = PAGE_COUNT;   /* the tile waiting to see a second tap */
static int64_t s_menu_held_us;

/* The pages the cycling saver leaves out: settings, because a slideshow
   should not land on a control panel, the message page when nothing has been
   sent, because "nothing sent yet" is not worth twenty seconds, and whatever
   has been struck off on the menu. */
static page_mask_t cycle_skip(void)
{
    page_mask_t skip = s_cycle_off;
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
    /* Clamped, because on_time() re-bases the clock part-way through a loop
       iteration whose `now` was captured at the top. See timecalc_since. */
    return timecalc_since(s_base_secs, s_base_us, now);
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
        /* "!zero" and "!newgame" change state that only PAGE_LEVEL boards
           can see or clear -- envio has PAGE_BUBBLE instead, with no way to
           re-zero or wipe her scoreboard from her own page, so letting these
           through anyway would silently shift her readout or blank her
           scoreboard from a stray BLE command. Gated on the availability
           mask rather than the board macro so it reads the same way
           home_page() already tests for "a board with the Feather's page". */
        if (s_pages.available & PAGE_BIT(PAGE_LEVEL)) {
            if (kind == UD_ZERO) zero_command(text);
            if (kind == UD_NEWGAME) {
                /* Wipe the scoreboard. A score set before the clock knew to
                   ask whether anyone was holding the board is not one anybody
                   made, and there was no way to clear it without a reflash. */
                s_hold_s = s_last_hold_s = s_prev_hold_s = s_best_hold_s = 0.0f;
                runs_save();
                ESP_LOGI(TAG, "scoreboard cleared");
            }
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
    if (s_pages.available & PAGE_BIT(PAGE_MENU)) vw_menu_tab(c);
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
        float ppm, se;
        int slip = (int)drift_slip_ms(&s_drift);
        /* The figure carries its own error bar. Without it a reader has no
           way to tell a crystal that is genuinely one ppm out from a fit that
           has not made up its mind, and those look identical on a panel. */
        if (drift_ppm_err(&s_drift, &ppm, &se))
            snprintf(buf, sizeof buf, "%+dms %+.1f~%.1fppm", slip,
                     (double)ppm, (double)se);
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

/* Station pressure means little away from where it was measured; forecasts
   and every other reading in the world are sea-level adjusted (MSLP), so a
   BME280's reading is corrected to match before it is stored or logged.
   Not guarded by CONFIG_SCREEN_ENV_ONLY: the heartbeat log below wants it on
   every board, not only envio's. ALT_M==0 (no fixed altitude configured)
   leaves it alone. */
static float env_msl(float station_hpa)
{
#if CONFIG_SCREEN_ALTITUDE_M > 0
    float msl = station_hpa / powf(1.0f - (float)CONFIG_SCREEN_ALTITUDE_M / 44330.0f, 5.255f);
#else
    float msl = station_hpa;
#endif
    /* A fixed trim (tenths of a hPa) to match a trusted reference: the BMP280
       carries a few hPa of absolute bias, so even at the right altitude the MSLP
       sits a little off. Calibrated once against a phone's sea-level reading. */
    return msl + (float)CONFIG_SCREEN_PRESSURE_CAL_X10 / 10.0f;
}

#if CONFIG_SCREEN_ENV_ONLY
/*
 * The room, once a minute, kept for thirty days.
 *
 * Sampling runs whatever page is showing, so turning to a chart shows a month
 * rather than an empty page filling up. It only starts once the date is
 * known, which on this board is at boot from the DS3231: a reading stamped
 * with time-since-power-on cannot be placed in a log that outlives the power.
 */
#define ENV_EVERY_MIN   5   /* one reading every 5 minutes */
#define ENV_DAY_MIN     (24 * 60)
#define ENV_MONTH_MIN   (30 * 24 * 60)

static envstore_t s_env;
static bool       s_env_ready;
static uint32_t   s_env_last_min;         /* the minute last recorded */
static bool       s_env_dirty = true;     /* the charts need rebuilding */

/* One sector at a time, so the walk allocates nothing and the stack stays
   out of it. */
static uint8_t s_env_buf[4096];

/* The newest reading, kept here so the clock page can show it without an I2C
   transaction or a flash read on every frame -- and it redraws thirty times a
   second for the milliseconds. The room moves once a minute at most. */
static env_sample_t s_env_now;
static bool         s_env_now_ok;

/*
 * The series a board may hold. Every board has an entry for each; a board
 * without the sensor simply never fills its own, and the page is not offered.
 * Fixed rather than per-board because the log is read back by tools and by
 * later firmware, and an index that means different things on different
 * boards is a trap set for one of them.
 */
#define ENV_TEMP 0
#define ENV_RH   1
#define ENV_HPA  2
#define ENV_VOC  3
#define ENV_CO2  4
#define ENV_SERIES 5

static envchart_t s_env_day[ENV_SERIES];
static envchart_t s_env_month[ENV_SERIES];
static envweek_t s_env_week[ENV_SERIES];

static bool s_env_gas;        /* a gas sensor answered at boot */

/* Which of the seven days a reading belongs to. Day six is today; day zero is
   six days before it. Older than that and it is not in this week. */
static int env_week_day(uint32_t minute, uint32_t newest)
{
    int32_t day = (int32_t)(minute / 1440u);
    int32_t today = (int32_t)(newest / 1440u);
    int32_t back = today - day;
    if (back < 0 || back > ENVWEEK_DAYS - 1) return -1;
    return ENVWEEK_DAYS - 1 - (int)back;
}

/* Minutes since 1970, from the date the Mac or the clock chip supplied and
   the board's own seconds. */
static bool env_now_minute(int64_t now, uint32_t *out)
{
    if (!s_synced || s_data.date_year < 2000) return false;
    int32_t days = timecalc_days(s_data.date_year, s_data.date_month,
                                 s_data.date_day);
    if (days < 0) return false;
    *out = (uint32_t)days * 1440u + now_secs(now) / 60u;
    return true;
}


#if CONFIG_SCREEN_ENV_DEMO
/*
 * A few days of invented indoor weather, so the charts can be looked at
 * before the sensors exist.
 *
 * Shaped rather than random, because random noise makes a chart that proves
 * nothing: a room warms through the afternoon and cools overnight, humidity
 * moves against the temperature because warm air holds more water, and VOCs
 * sit at a low baseline with spikes around cooking and cleaning. A chart of
 * that tells you whether the axes, the binning and the min-max bands are
 * right. A chart of white noise tells you only that something drew.
 *
 * Written backwards from now, one a minute, and every record marked
 * ENV_SYNTHETIC so no one -- including a later me -- mistakes it for a
 * measurement.
 */
#define ENV_DEMO_DAYS 3

static int32_t demo_noise(uint32_t seed)
{
    /* A cheap reproducible LCG: the same fill twice gives the same chart,
       which matters when comparing a change to the drawing code. */
    seed = seed * 1103515245u + 12345u;
    /* A tenth of a degree, not half of one. The first attempt used +/-50
       hundredths, which on a chart whose whole range is five degrees drew a
       band a fifth of the panel high in every column -- a fuzzy caterpillar
       rather than a room. Real rooms are smooth; the noise has to be small
       enough that the shape is what you see. */
    return (int32_t)((seed >> 16) % 21) - 10;       /* -10..+10 */
}

static void env_demo_fill(uint32_t now_minute)
{
    if (!s_env_ready) return;
    if (envstore_count(&s_env) > 0) return;        /* never over real data */
    if (aht21_present() || bme280_present() || ens160_present()) return;

    const int total = ENV_DEMO_DAYS * 24 * 60;
    ESP_LOGW(TAG, "no sensors: filling the log with %d INVENTED readings", total);

    for (int i = total; i > 0; i--) {
        uint32_t minute = now_minute - (uint32_t)i;
        int mins_of_day = (int)(minute % 1440u);
        /* Peak in the late afternoon, trough before dawn. */
        float phase = (float)(mins_of_day - 300) / 1440.0f * 6.2831853f;
        float warm = sinf(phase);

        env_sample_t r;
        memset(&r, 0, sizeof r);
        r.minute = minute;
        r.temp_c100 = (int16_t)(2150.0f + 250.0f * warm + demo_noise((uint32_t)i));
        r.rh_c100 = (uint16_t)(4800.0f - 600.0f * warm + demo_noise((uint32_t)i * 7u));
        r.flags = ENV_HAVE_TEMP | ENV_HAVE_RH | ENV_HAVE_GAS | ENV_SYNTHETIC;

        /* A baseline with spikes at breakfast and at dinner, because that is
           what a kitchen does to a VOC sensor. */
        int tvoc = 90 + (int)(20.0f * warm) + demo_noise((uint32_t)i * 13u) / 4;
        int hour = mins_of_day / 60;
        if (hour == 8 || hour == 18) tvoc += 250 + demo_noise((uint32_t)i * 3u) * 2;
        if (tvoc < 0) tvoc = 0;
        r.tvoc_ppb = (uint16_t)tvoc;
        r.eco2_ppm = (uint16_t)(420 + tvoc / 2);
        r.aqi = (uint8_t)(tvoc < 150 ? 1 : tvoc < 300 ? 2 : tvoc < 500 ? 3 : 4);

        if (!envstore_add(&s_env, &r)) {
            ESP_LOGW(TAG, "demo fill stopped after %d readings", total - i);
            break;
        }
    }
    s_env_dirty = true;
    s_env_now_ok = envstore_latest(&s_env, &s_env_now);
    s_env_last_min = now_minute;
    ESP_LOGW(TAG, "log now holds %d readings, all invented",
             envstore_count(&s_env));
}
#endif

/*
 * Reads every environment sensor on the bus and folds the two that overlap
 * into an average, filling `rec` (all but its timestamp, which the caller
 * sets). Returns false when nothing measured the room.
 *
 * With both an AHT21 and a BME280 on the bus neither one's microclimate is
 * more right than the other's, so temperature -- and humidity, when the BME is
 * a real humidity part -- is the mean of the two rather than a pick between
 * them. Shared by the once-a-minute flash sampler and envo's 30-second SD log
 * so the two never disagree: both fold the same reading the same way.
 */
static bool env_read_averaged(env_sample_t *rec)
{
    float at = 0, arh = 0;
    bool have_aht = aht21_present() && aht21_read(&at, &arh);

    float bt = 0, brh = 0, hpa = 0;
    bool have_bme = bme280_read(&bt, &hpa, &brh);
    /* A BMP280 (id 0x58) is temperature+pressure only -- bme280_read hands
       back 0.0 for its humidity, not a reading. Averaging that in would halve
       a real AHT21 number every time both answer, so only a BME280 proper
       (0x60/0x61) contributes to the humidity side of the average. */
    bool have_bme_rh = have_bme && bme280_id() != 0x58;

    float t = 0, rh = 0;
    bool have_th = have_aht || have_bme;
    if (have_aht && have_bme)    { t = (at + bt) / 2.0f; }
    else if (have_aht)           { t = at; }
    else if (have_bme)           { t = bt; }
    if (have_aht && have_bme_rh) { rh = (arh + brh) / 2.0f; }
    else if (have_aht)           { rh = arh; }
    else if (have_bme_rh)        { rh = brh; }

    if (have_bme) {
        rec->hpa_x10 = (uint16_t)(env_msl(hpa) * 10.0f);
        rec->flags |= ENV_HAVE_HPA;
    }

    if (!have_th) return false;
    rec->temp_c100 = (int16_t)(t * 100.0f);
    rec->rh_c100 = (uint16_t)(rh * 100.0f);
    rec->flags |= ENV_HAVE_TEMP;
    /* Only claim humidity when a humidity sensor actually answered: a BMP280
       (no RH) would otherwise chart a fabricated 0%. */
    if (have_aht || have_bme_rh) rec->flags |= ENV_HAVE_RH;

    if (ens160_present()) {
        /*
         * Tell it the air it is sitting in before asking what is in the air:
         * without compensation a MOX sensor's readings wander with the
         * weather. Then take the reading and keep the chip's own opinion of
         * how much it is worth.
         */
        ens160_compensate(t, rh);

        uint16_t eco2, tvoc;
        uint8_t aqi;
        ens160_validity_t validity;
        if (ens160_read(&eco2, &tvoc, &aqi, &validity)) {
            rec->tvoc_ppb = tvoc;
            rec->eco2_ppm = eco2;
            rec->aqi = aqi;
            rec->flags |= ENV_HAVE_GAS | ENV_GAS_FLAGS(validity);
        }
    }
    return true;
}

static void env_sample(int64_t now)
{
    if (!s_env_ready) return;
    uint32_t minute;
    if (!env_now_minute(now, &minute)) return;
#if CONFIG_SCREEN_ENV_DEMO
    /* Here rather than at boot: the fill needs real timestamps, and on a
       board with no clock chip the time does not exist until a Mac says so. */
    {
        static bool filled;
        if (!filled) { filled = true; env_demo_fill(minute); }
    }
#endif
    /*
     * Signed, deliberately. Unsigned, a clock that moved backwards makes this
     * difference enormous rather than negative, the guard waves it through,
     * and the bad reading then becomes the baseline -- so the next good one
     * looks like a jump too and gets written as well. One corrupt timestamp
     * turned into a duplicate pair in the log that way.
     */
    if (s_env_last_min != 0) {
        int32_t since = (int32_t)(minute - s_env_last_min);
        if (since >= 0 && since < ENV_EVERY_MIN) return;
        if (since < 0)
            ESP_LOGW(TAG, "the clock moved back %d minutes; logging from here",
                     -since);
    }

    env_sample_t rec;
    memset(&rec, 0, sizeof rec);
    rec.minute = minute;

    /* Nothing measured the room, so there is nothing to record. A row of
       zeros would chart as a real reading of zero. */
    if (!env_read_averaged(&rec)) return;

    if (envstore_add(&s_env, &rec)) {
        s_env_last_min = minute;
        s_env_dirty = true;
        s_env_now = rec;
        s_env_now_ok = true;
    }
}

#if CONFIG_SCREEN_BOARD_TOUCH_LCD_147
/*
 * envo's long-term log: one CSV line every 30 s to the microSD, in a file per
 * day (envo/YYYY-MM-DD.csv). Independent of the flash ring above -- a different
 * cadence on a different medium -- but it logs the SAME averaged reading that
 * env_read_averaged() produces, so the card and the on-screen charts agree to
 * the digit. Runs only once the DS3231 has a real date, so every line carries
 * a true wall-clock timestamp. Three months at 30 s is ~260k lines, ~13 MB --
 * nothing on a card, so no pruning.
 */
static void env_sdlog(int64_t now)
{
    static int64_t last_us = 0;
    if (last_us != 0 && now - last_us < 30LL * 1000 * 1000) return;

    ds3231_date_t date;
    uint32_t sod;
    if (!ds3231_read_date(&date) || date.year < 2000 || !ds3231_read(&sod))
        return;                       /* no trustworthy clock yet; try again */
    last_us = now;                    /* attempt at most once per 30 s */

    env_sample_t rec;
    memset(&rec, 0, sizeof rec);
    if (!env_read_averaged(&rec)) return;

    char path[48];
    snprintf(path, sizeof path, "envo/%04d-%02d-%02d.csv",
             date.year, date.month, date.day);

    if (!sd_exists(path)) {
        static const char hdr[] =
            "timestamp,temp_c,rh_pct,pressure_hpa,tvoc_ppb,eco2_ppm\n";
        sd_append(path, hdr, sizeof hdr - 1);
    }

    int hh = (int)(sod / 3600) % 24, mm = (int)(sod / 60) % 60, ss = (int)(sod % 60);
    char line[128];
    int n = snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02d,",
                     date.year, date.month, date.day, hh, mm, ss);
    n += snprintf(line + n, sizeof line - n, "%.2f,", rec.temp_c100 / 100.0f);
    if (rec.flags & ENV_HAVE_RH)
        n += snprintf(line + n, sizeof line - n, "%.2f,", rec.rh_c100 / 100.0f);
    else
        n += snprintf(line + n, sizeof line - n, ",");
    if (rec.flags & ENV_HAVE_HPA)
        n += snprintf(line + n, sizeof line - n, "%.1f,", rec.hpa_x10 / 10.0f);
    else
        n += snprintf(line + n, sizeof line - n, ",");
    if (rec.flags & ENV_HAVE_GAS)
        n += snprintf(line + n, sizeof line - n, "%u,%u\n",
                      (unsigned)rec.tvoc_ppb, (unsigned)rec.eco2_ppm);
    else
        n += snprintf(line + n, sizeof line - n, ",\n");

    esp_err_t err = sd_append(path, line, (size_t)n);
    static bool announced = false;
    if (!announced) {
        announced = true;
        ESP_LOGI(TAG, "SD env log -> /sdcard/%s (%s)", path,
                 err == ESP_OK ? "written" : esp_err_to_name(err));
    }
}
#endif /* CONFIG_SCREEN_BOARD_TOUCH_LCD_147 */

/* Folding the log into the charts. One walk fills all six: reading the
   partition six times to draw three pages would be six times the flash
   traffic for the same answer. */
typedef struct { uint32_t newest; } env_fold_t;

static bool env_fold(const env_sample_t *r, void *ctx)
{
    const env_fold_t *f = ctx;
    uint32_t age = f->newest >= r->minute ? f->newest - r->minute : 0;

    int16_t v[ENV_SERIES] = {
        r->temp_c100, (int16_t)r->rh_c100, (int16_t)r->hpa_x10,
        (int16_t)r->tvoc_ppb, (int16_t)r->eco2_ppm,
    };

    int wd = env_week_day(r->minute, f->newest);
    if (wd >= 0)
        for (int i = 0; i < ENV_SERIES; i++) envweek_add(&s_env_week[i], wd, v[i]);

    if (age < ENV_DAY_MIN) {
        /* Newest at the right-hand edge, so the chart grows leftwards into
           the past the way every other chart here does. */
        int col = ENVCHART_COLS - 1 - (int)((age * ENVCHART_COLS) / ENV_DAY_MIN);
        for (int i = 0; i < ENV_SERIES; i++) envchart_add(&s_env_day[i], col, v[i]);
    }
    if (age < ENV_MONTH_MIN) {
        int col = ENVCHART_COLS - 1 - (int)((age * ENVCHART_COLS) / ENV_MONTH_MIN);
        for (int i = 0; i < ENV_SERIES; i++) envchart_add(&s_env_month[i], col, v[i]);
    }
    return true;
}

static void env_rebuild(void)
{
    if (!s_env_dirty || !s_env_ready) return;
    s_env_dirty = false;

    for (int i = 0; i < ENV_SERIES; i++) {
        envchart_reset(&s_env_day[i]);
        envchart_reset(&s_env_month[i]);
        envweek_reset(&s_env_week[i]);
    }
    env_sample_t newest;
    if (!envstore_latest(&s_env, &newest)) return;

    /* The day initials, worked out from the newest reading's date rather
       than from the board's idea of today: the log is what is being labelled,
       and after a week unplugged those are not the same thing. */
    {
        int32_t today = (int32_t)(newest.minute / 1440u);
        for (int d = 0; d < ENVWEEK_DAYS; d++) {
            int32_t days = today - (ENVWEEK_DAYS - 1 - d);
            char initial = "SMTWTFS"[timecalc_weekday(days)];
            for (int i = 0; i < ENV_SERIES; i++) envweek_label(&s_env_week[i], d, initial);
        }
    }
    env_fold_t f = { newest.minute };
    envstore_walk(&s_env, s_env_buf, env_fold, &f);
}

/* What each page is called, what colour it draws in, and how to write its
   numbers. Pressure moves by tenths and wants no decimals at all on a panel
   this wide; temperature earns one. */
static void env_fmt_temp(int16_t raw, char *out, int size)
{ snprintf(out, size, "%.0f", (double)raw / 100.0); }
static void env_fmt_rh(int16_t raw, char *out, int size)
{ snprintf(out, size, "%.0f", (double)raw / 100.0); }
static void env_fmt_hpa(int16_t raw, char *out, int size)
{ snprintf(out, size, "%.0f", (double)raw / 10.0); }
/* Parts per billion, whole numbers: tenths of a ppb is a precision a MOX
   sensor does not have and an axis cannot show. */
static void env_fmt_tvoc(int16_t raw, char *out, int size)
{ snprintf(out, size, "%d", (int)(uint16_t)raw); }

static void env_fmt_co2(int16_t raw, char *out, int size)
{ snprintf(out, size, "%d", (int)(uint16_t)raw); }

static void env_format(int which, int16_t raw, char *out, int size)
{
    switch (which) {
    case ENV_TEMP: snprintf(out, size, "%.1fC", (double)raw / 100.0); break;
    case ENV_RH:   snprintf(out, size, "%.0f%%", (double)raw / 100.0); break;
    case ENV_HPA:  snprintf(out, size, "%.0f", (double)raw / 10.0); break;
    case ENV_VOC:  snprintf(out, size, "%dppb", (int)(uint16_t)raw); break;
    default:       snprintf(out, size, "%dppm", (int)(uint16_t)raw); break;
    }
}

/* How long each reading holds the week page before the next takes over.
   Short enough that a glance catches more than one, long enough to read. */
#define WEEK_DWELL_US (6 * 1000000LL)

static void draw_week(canvas_t *c, int64_t now)
{
    static const char *titles[3] = { "TEMP WEEK", "HUMIDITY WEEK", "PRESSURE WEEK" };
    static const uint16_t colours[3] = { PAL_A1, PAL_A0, PAL_A2 };

    /* One page for all three, turning over by itself: three pages of this in
       the rotation made a loop nobody waits out. */
    int which = (int)((now / WEEK_DWELL_US) % 3);

    env_rebuild();

    /* The week's own extremes in the title, since the bars share one scale
       and the numbers on it are not otherwise written anywhere. */
    char value[24] = "--";
    int16_t lo, hi;
    if (envweek_range(&s_env_week[which], &lo, &hi)) {
        char a[12], b[12];
        env_format(which, lo, a, sizeof a);
        env_format(which, hi, b, sizeof b);
        snprintf(value, sizeof value, "%s-%s", a, b);
    }
    envweek_draw(c, titles[which], value, colours[which], &s_env_week[which]);
    display_blit();
}

/*
 * Temperature and humidity together, across the day. They move against each
 * other -- warm air holds more water, so a room warming usually shows the two
 * traces diverging -- and that relationship is invisible when each has its
 * own page.
 */
static void draw_trend(canvas_t *c)
{
    env_rebuild();

    char value[24] = "--";
    env_sample_t latest;
    if (s_env_ready && envstore_latest(&s_env, &latest))
        snprintf(value, sizeof value, "%.1fC %.0f%%",
                 (double)latest.temp_c100 / 100.0, (double)latest.rh_c100 / 100.0);

    char footer[64];
    int16_t tlo, thi, hlo, hhi;
    bool ht = envchart_range(&s_env_day[0], &tlo, &thi);
    bool hh = envchart_range(&s_env_day[1], &hlo, &hhi);
    if (ht && hh)
        snprintf(footer, sizeof footer, "%.1f-%.1fC  %.0f-%.0f%%",
                 (double)tlo / 100.0, (double)thi / 100.0,
                 (double)hlo / 100.0, (double)hhi / 100.0);
    else
        snprintf(footer, sizeof footer, "%s",
                 s_env_ready ? "logging; nothing charted yet" : "no log partition");

    envpair_draw(c, "TEMP + RH", value, footer,
                 &s_env_day[0], PAL_A1, &s_env_day[1], PAL_A0);
    display_blit();
}

static const char *const env_titles[ENV_SERIES] = {
    "TEMP", "HUMIDITY", "PRESSURE", "AIR VOC", "CO2e",
};
static const uint16_t env_colours[ENV_SERIES] = {
    PAL_A1, PAL_A0, PAL_A2, PAL_A4, PAL_A3,
};
static const envfmt_fn env_fmts[ENV_SERIES] = {
    env_fmt_temp, env_fmt_rh, env_fmt_hpa, env_fmt_tvoc, env_fmt_co2,
};

/*
 * Builds the page and its title-bar value for one reading. env_rebuild() must
 * have run. The title carries the reading now; with `full` it adds the month's
 * range, which the single-chart page has room for but the stacked pair does
 * not. The VOC value carries the chip's air-quality word instead: "good" says
 * something a count of parts per billion does not.
 */
static void env_page(int which, envpage_t *p, char *value, size_t vsz, bool full)
{
    snprintf(value, vsz, "--");
    p->has_latest = false;   /* set true below if a reading is available */
    env_sample_t latest;
    int16_t mlo, mhi;
    bool have_month = envchart_range(&s_env_month[which], &mlo, &mhi);

    if (s_env_ready && envstore_latest(&s_env, &latest)) {
        int16_t raw[ENV_SERIES] = {
            latest.temp_c100, (int16_t)latest.rh_c100, (int16_t)latest.hpa_x10,
            (int16_t)latest.tvoc_ppb, (int16_t)latest.eco2_ppm,
        };
        p->latest = raw[which];
        p->has_latest = true;

        char now_s[16];
        env_format(which, raw[which], now_s, sizeof now_s);

        if (which == ENV_VOC) {
            const char *note = ENV_GAS_VALIDITY(latest.flags) == 0
                             ? ens160_aqi_name(latest.aqi) : "settling";
            snprintf(value, vsz, "%s  %s", now_s, note);
        } else if (full && have_month) {
            char a[16], b[16];
            env_format(which, mlo, a, sizeof a);
            env_format(which, mhi, b, sizeof b);
            snprintf(value, vsz, "%s  30d %s-%s", now_s, a, b);
        } else {
            snprintf(value, vsz, "%s", now_s);
        }
    }

    p->title = env_titles[which];
    p->value = value;
    p->colour = env_colours[which];
    p->fmt = env_fmts[which];
    p->recent = &s_env_day[which];
    p->longer = &s_env_month[which];
    p->span_minutes = ENV_DAY_MIN;
    p->end_minute = s_env_now_ok ? (int)(s_env_now.minute % 1440u) : -1;

    /*
     * Reference lines for the air pages, so the trace reads as a place on a
     * scale, not just a number. TVOC in ppb and eCO2 in ppm: below the low
     * line is fine, above the high one is time to open a window. Values are
     * common indoor-air guidance, not the sensor's exact bands.
     */
    static const env_thresh_t voc_thresh[] = {
        { 300, "ok", false }, { 1000, "vent", true },
    };
    static const env_thresh_t co2_thresh[] = {
        { 800, "ok", false }, { 1200, "vent", true },
    };
    p->thresh = NULL;
    p->thresh_n = 0;
    /* Gas concentrations get a log axis and a danger line; the trace bunches
       against a linear scale and a wide-open room can spike a decade. */
    p->log_scale = (which == ENV_VOC || which == ENV_CO2);
    if (which == ENV_VOC)      { p->thresh = voc_thresh; p->thresh_n = 2; }
    else if (which == ENV_CO2) { p->thresh = co2_thresh; p->thresh_n = 2; }
}

static void draw_room(canvas_t *c, int which)
{
    env_rebuild();
    static char value[72];
    envpage_t p;
    env_page(which, &p, value, sizeof value, true);
    envpage_draw(c, &p);
    display_blit();
}

/* Two related readings stacked to fill the tall portrait panel. */
static void draw_pair(canvas_t *c, int top, int bottom)
{
    env_rebuild();
    static char va[40], vb[40];
    envpage_t a, b;
    env_page(top, &a, va, sizeof va, false);
    env_page(bottom, &b, vb, sizeof vb, false);
    env2_draw(c, &a, &b);
    display_blit();
}

static void draw_climate(canvas_t *c) { draw_pair(c, ENV_TEMP, ENV_RH); }
static void draw_air(canvas_t *c)     { draw_pair(c, ENV_VOC, ENV_CO2); }
#endif /* CONFIG_SCREEN_ENV_ONLY */

/* One line of text, centred on the page. */
static void centred(canvas_t *c, int row, const char *s, uint16_t colour)
{
    int col = (c->cols - (int)strlen(s)) / 2;
    canvas_puts(c, col < 0 ? 0 : col, row, s, colour);
}

/*
 * A panel too narrow to hold the weather on one line. Twenty-six columns is
 * one; the CrowPanel's sixty-four is not, and it keeps the layout it has.
 */
/* "Narrow" means the weather does not fit on one line, so the digits pin under
   the date and the weather gets the rows below rather than two crammed at the
   foot. The weather runs to 62 characters (weather.py caps it), so only a panel
   that wide reads as wide -- the CrowPanel's 64 does, envio's 40 does not, even
   landscape. Below 40 was the old test, from when 26 and 64 were the only widths
   and nothing sat between them; envio at exactly 40 fell on the wrong side and
   truncated the day's low. */
static bool clock_narrow(const canvas_t *c) { return c->cols < 62; }

/*
 * Where the digits go, and how many rows they take.
 *
 * Centred, they sit in the middle of the panel and leave two rows underneath.
 * The weather is a sentence of about fifty-five characters -- conditions,
 * temperature, apparent temperature, humidity, wind, and the day's high and
 * low -- which is three lines at twenty-six columns, so two rows silently ate
 * the end of it, and the end of it is the day's low.
 *
 * So on a narrow panel the digits are pinned directly under the date instead,
 * and everything below them belongs to the weather. Nothing is centred away
 * from the text it has to share the panel with.
 */
static int clock_face(canvas_t *c, const char *buf)
{
    int w, h;
    canvas_big_size(c, buf, &w, &h);
    if (!clock_narrow(c)) { canvas_big(c, buf); return 0; }

    canvas_big_at(c, buf, (c->w - w) / 2, 2 * c->cell_h);
    int rows = (h + c->cell_h - 1) / c->cell_h;
    return 2 + rows;                 /* the first row the digits do not use */
}

/*
 * Date above the digits, weather below, both sent from the Mac.
 *
 * `first_row` is where the weather may start; 0 means "the usual place", two
 * rows up from the bottom, which is what a panel wide enough for one line
 * wants. Wrapped with textwrap rather than by hand, because textwrap marks a
 * truncation with an ellipsis: the previous version of this dropped the tail
 * off the right-hand edge with no sign that it had, which is how the day's
 * low went missing twice without the page looking wrong.
 */
/*
 * The room, in one line, from the newest logged reading. Empty on a board
 * with no sensor, which is every board but wave.
 */
static bool clock_room_line(char *out, int size)
{
#if CONFIG_SCREEN_ENV_ONLY
    if (!s_env_now_ok) return false;

    /*
     * Only what was actually measured. The reading carries flags saying which
     * sensors were fitted when it was taken, and a board with no barometer was
     * printing "0.0hPa" -- a number that looks like a measurement, is not one,
     * and is not even a plausible pressure.
     *
     * Where there is no barometer but there is a gas sensor, the third slot
     * goes to the VOCs, which is what that board is for.
     */
    char tail[24] = "";
    if (s_env_now.flags & ENV_HAVE_HPA)
        snprintf(tail, sizeof tail, "  %.0fhPa", (double)s_env_now.hpa_x10 / 10.0);
    else if (s_env_now.flags & ENV_HAVE_GAS)
        snprintf(tail, sizeof tail, "  %uppb", (unsigned)s_env_now.tvoc_ppb);

    snprintf(out, size, "%.1fC  %.0f%%%s",
             (double)s_env_now.temp_c100 / 100.0,
             (double)s_env_now.rh_c100 / 100.0, tail);
    return true;
#else
    (void)out; (void)size;
    return false;
#endif
}

static void draw_clock_extras(canvas_t *c, int first_row)
{
    if (s_data.date[0]) centred(c, 1, s_data.date, PAL_FG);

    int top = first_row > 0 ? first_row : c->rows - 2;
    int last = c->rows - 1;
    if (top > last) return;

    char room[40];
    bool have_room = clock_room_line(room, sizeof room);
    bool have_wx = s_data.weather[0] != '\0';

    /*
     * With both, the forecast goes under the digits and the room sits on the
     * bottom row: the weather is several lines and wants the room, the
     * readings are one line and want to be where the eye lands last. With
     * only the readings they go directly under the digits rather than leaving
     * a gap and sitting alone at the foot of the panel.
     */
    if (have_room && !have_wx) {
        centred(c, top, room, PAL_A2);
        return;
    }
    if (have_room) {
        centred(c, last, room, PAL_A2);
        last--;
    }
    if (!have_wx) return;

    int avail = last - top + 1;
    if (avail < 1) return;
    if (avail > TW_MAX_LINES) avail = TW_MAX_LINES;

    int cols = c->cols < TW_MAX_COLS ? c->cols : TW_MAX_COLS;
    static char lines[TW_MAX_LINES][TW_MAX_COLS + 1];
    size_t n = textwrap_fields(s_data.weather, (size_t)cols, (size_t)avail, lines);
    for (size_t i = 0; i < n; i++)
        centred(c, top + (int)i, lines[i], PAL_A0);
}

/* Draws the clock somewhere new each minute. Deliberately not random per
   frame: it must hold still while you read it. */
static void draw_saver(canvas_t *c, uint32_t secs)
{
    /* Clear first: the saver draws only the drifting time, and without this it
       left the page underneath showing around it -- on envio's wide landscape
       that was a whole room chart bleeding through beside the clock. */
    canvas_clear(c);

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
            canvas_clear(c);
            draw_clock_extras(c, clock_face(c, "--:--:--"));
            if (s_pages.available & PAGE_BIT(PAGE_MENU)) vw_menu_tab(c);
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
    /* Wipe the whole canvas, not just the digits: the clock redraws only its
       own area, so without this the page underneath (a room chart) stays frozen
       on the rest of the screen while only the time ticks. Double-buffered
       through the rotation, so a full clear each second does not flicker. */
    canvas_clear(c);
    draw_clock_extras(c, clock_face(c, buf));
    if (s_pages.available & PAGE_BIT(PAGE_MENU)) vw_menu_tab(c);
    display_blit();
    s_drawn_second = (int)secs;
}

/*
 * The multi-day forecast, one day a row, pushed from the Mac (push-clock.sh).
 * Each line is already formatted there, so this only lays them out -- today
 * bright at the top, the rest dimmer, a blank row between on the tall panel.
 */
static void draw_forecast(canvas_t *c)
{
    canvas_clear(c);
    canvas_fill_rect(c, 0, 0, c->w, c->cell_h, PAL_TITLE_BG);
    canvas_puts(c, 1, 0, "FORECAST", PAL_FG);

    int row = 2, shown = 0;
    for (int i = 0; i < UD_FC_DAYS && row < c->rows; i++) {
        if (s_data.forecast[i][0] == '\0') continue;
        /* Today in white, the rest in amber -- the dim grey was too dark to
           read on this panel. Still a clear hierarchy, both legible. */
        canvas_puts(c, 1, row, s_data.forecast[i], i == 0 ? PAL_FG : PAL_A1);
        row += 2;
        shown++;
    }
    if (shown == 0) centred(c, c->rows / 2, "no forecast yet", PAL_DIM);
    display_blit();
}



void app_main(void)
{
    if (display_init() != ESP_OK) {
        /* A panel that failed to start cannot report its own failure. */
        ESP_LOGE(TAG, "display init failed; halting");
        return;
    }

#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
    /* One-time verification that the PMIC's gauge answers plausibly; not a
       per-frame log. Safe to remove once confirmed against real hardware. */
    {
        axp2101_batt_t batt;
        if (axp2101_battery(&batt)) {
            ESP_LOGI(TAG, "battery: %d%% %dmV charge=%d vbus=%d",
                     batt.percent, batt.millivolts, (int)batt.charge, batt.vbus);
        } else {
            ESP_LOGW(TAG, "battery: gauge did not answer");
        }
    }
#endif

    bool touch = false, big = false;
#if HAVE_BUTTONS
    /* Losing the buttons costs navigation, not the display, so carry on. */
    if (buttons_init() != ESP_OK) ESP_LOGW(TAG, "buttons unavailable");
#endif
#ifdef CONFIG_SCREEN_BOARD_CROWPANEL_7
    big = true;              /* the 800x480 layout, which is a separate thing */
#endif
#if HAVE_TOUCH
    touch = touch_init() == ESP_OK;
#endif
#if TOUCH_BY_HALVES
    /*
     * A tap on the left half goes back and the right half forward, so there
     * is no menu to open and no tab to open it with. pages_back was written
     * for exactly this and had never been wired to a board.
     */
    vw_set_menu_tab(false);
#else
    /* No finger, no MENU tab: it cannot be pressed, and draw_message paints
       it over the text's first six columns. */
    vw_set_menu_tab(touch);
#endif
    /* Which pages this board offers: the data pages need the big panel, and
       the menu and settings need a finger. */
    page_mask_t available = 0;
    for (int i = 0; i < PAGE_COUNT; i++) {
        const page_def_t *pd = &page_defs[i];
        if (!(pd->where & (big ? PG_BIG : PG_SMALL))) continue;
        if (pd->needs_touch && !touch) continue;
        available |= PAGE_BIT(i);
    }
#if TOUCH_BY_HALVES
    /* Nothing to navigate to a menu with, and a settings page that could only
       be reached by paging past it. */
    available &= ~(PAGE_BIT(PAGE_MENU) | PAGE_BIT(PAGE_SETTINGS));
#endif
#if CONFIG_SCREEN_ENV_ONLY
    /* Probe the room sensors before the page list is built just below: the VOC,
       CO2 and pressure pages are each offered only if their sensor actually
       answered, and deciding that needs the sensor known here, not after. The
       gas sensor being detected after the list was built is exactly why the
       air-quality and CO2 pages were missing. The shared bme280_init later is
       skipped for this board so it is not probed twice. */
    bme280_init();
    aht21_init();
    ens160_init();
    s_env_gas = ens160_present();

    /* envio is a camera app now (2026-09-19): Camera (home), Gallery, the
       Level and the Clock. The weather/air/climate/system dashboard that
       used to live here is gone from the page list -- env_sample() below
       keeps logging the room sensors regardless, so the data is not lost,
       just not shown. Camera/Gallery/Bubble are added back in further down,
       after their own sensor/board guards; only the clock survives this
       mask untouched. */
    available &= PAGE_BIT(PAGE_CLOCK);
#else
    /* The forecast belongs to the weather dashboard (envio), not to lilly's
       clock/usage slideshow -- strip it here or a small non-ENV panel offers
       it too. */
    available &= ~(PAGE_BIT(PAGE_ROOM_TEMP) | PAGE_BIT(PAGE_ROOM_RH)
                 | PAGE_BIT(PAGE_ROOM_HPA) | PAGE_BIT(PAGE_ROOM_VOC)
                 | PAGE_BIT(PAGE_ROOM_CO2) | PAGE_BIT(PAGE_TREND)
                 | PAGE_BIT(PAGE_WEEK) | PAGE_BIT(PAGE_FORECAST));
#endif
    /* Both IMU pages need a sensor, which the page table cannot know about:
       it describes boards, and this is a question about what is plugged into
       one today. A board compiled without either never offers them at all. */
    page_mask_t imu_pages = PAGE_BIT(PAGE_LEVEL) | PAGE_BIT(PAGE_PARTICLES)
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
    /* envio's bubble level needs the sensor too, and is added after the
       ENV_ONLY mask above and the imu_pages strip so it is not immediately
       cleared by either -- it is not one of the "both IMU pages" above and
       does not want to be struck off with them. */
#if HAVE_IMU
    if (s_imu) available |= PAGE_BIT(PAGE_BUBBLE);
#endif
    /* The System page (uptime, battery, free RAM) was part of the old
       weather/air dashboard and is dropped now that envio is a camera app;
       draw_system() and CONFIG_SCREEN_BOARD_TOUCH_LCD_35B's PAGE_SYSTEM slot
       stay in the tree (harmless, just never made available) rather than
       being ripped out along with the pages the task asked to keep. */
    /* The camera page only exists on the board with the OV5640. Its
       page_def_t has needs_touch=false (a swipe reaches it, no menu tile),
       so the generic where/needs_touch loop above adds it to every board
       unconditionally -- it has to be struck off explicitly here or every
       other board gets a page that draws nothing but a clear screen, the
       same trap HAVE_PIP's #else guards against below. */
#if CONFIG_SCREEN_HAVE_CAMERA
    available |= PAGE_BIT(PAGE_CAMERA) | PAGE_BIT(PAGE_GALLERY);
#else
    available &= ~(PAGE_BIT(PAGE_CAMERA) | PAGE_BIT(PAGE_GALLERY));
#endif
#if HAVE_PIP
    /* envio is Pip's board: the face is home, with the clock and BLE messages
       still reachable behind it. The data pages want a panel she does not have,
       and her other subsystems become their own apps in later increments. Pip
       needs the IMU; without it she falls back to the clock. */
    if (!s_imu) available &= ~PAGE_BIT(PAGE_PIP);
    available &= PAGE_BIT(PAGE_PIP) | PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MESSAGE);
#else
    available &= ~PAGE_BIT(PAGE_PIP);
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
#if CONFIG_SCREEN_ENV_ONLY
    /*
     * Nothing on this board moves on its own. The pages do not rotate, and
     * the screensaver drifts the clock about rather than running a slideshow
     * of its own -- a cycling saver after ten minutes idle would be exactly
     * the carousel the rotation was turned off to stop. A button is the only
     * thing that changes the page.
     */
    s_settings.saver_cycle = false;
#endif
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
       is plugged in but silent is distinguishable from one that is absent.
       On an ENV_ONLY board these were already brought up before the page list
       was built, so only the other boards probe here. */
#if !CONFIG_SCREEN_ENV_ONLY
    bme280_init();
#endif
#if CONFIG_SCREEN_ENV_ONLY
    {
        static envflash_t flash;
        s_env_ready = envflash_open(&flash) && envstore_open(&s_env, &flash);
        /* Whatever the last run recorded, so the clock page has numbers
           before this run's first sample a minute from now. */
        if (s_env_ready) s_env_now_ok = envstore_latest(&s_env, &s_env_now);
        if (s_env_ready)
            ESP_LOGI(TAG, "room log: %d of %d readings kept (%d days at one a minute)",
                     envstore_count(&s_env), envstore_capacity(&s_env),
                     envstore_capacity(&s_env) / (24 * 60));
        else
            ESP_LOGW(TAG, "room log unavailable; the charts will stay empty");
    }
#endif
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
#if HAVE_PIP
    if (s_imu) {
        pip_init(&s_pip);
        shakedet_init(&s_pip_shake);
        ESP_LOGI(TAG, "pip awake on %dx%d", c->w, c->h);
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
                   || s_pages.current == PAGE_BUBBLE
                   || s_pages.current == PAGE_PARTICLES))
            period_us = 33000;      /* 30 fps while the sensor drives it */
#endif
#if HAVE_PARTICLES
        if (s_imu && s_pages.current == PAGE_PARTICLES)
            period_us = 33000;      /* the liquid wants every frame it can get */
#endif
#if HAVE_PIP
        if (s_pages.current == PAGE_PIP)
            period_us = 33000;      /* 30 fps so the eyes move smoothly */
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

        if (touch && touch_tapped()) {
            int tx, ty, row, choice;
            page_t target;
            touch_point(&tx, &ty);
            if (s_saver) {
                /* The first tap dismisses the saver rather than also changing
                   the page, which would be a surprise. */
                s_pages.last_activity_us = now;
                ESP_LOGI(TAG, "tap at %d,%d -> wake", tx, ty);
            } else if ((s_pages.available & PAGE_BIT(PAGE_MENU))
                       && s_pages.current != PAGE_MENU && vw_menu_tab_hit(c, tx, ty)) {
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
#if HAVE_LEVEL
            } else if (s_pages.current == PAGE_BUBBLE) {
                /* A tap on the Level page restarts the current attempt
                   rather than paging away -- swipes still page, below. */
                s_hold_s = 0.0f;
                s_armed = false;
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "tap at %d,%d -> level reset", tx, ty);
#endif
#if CONFIG_SCREEN_HAVE_CAMERA
            } else if (s_pages.current == PAGE_CAMERA && in_res_button(c, tx, ty)) {
                camera_cycle_resolution();
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "tap -> resolution now %s",
                         res_label(camera_get_capture_size()));
            } else if (s_pages.current == PAGE_CAMERA && in_shot_button(c, tx, ty)) {
                /* The deinit + reinit + settling + JPEG encode + SD write takes
                   on the order of a second, so paint something before it or the
                   screen just freezes on the last preview frame. */
                canvas_puts_px(c, 6, 34, "capturing...", PAL_A1);
                display_blit();
                camera_shutter();
                s_pages.last_activity_us = esp_timer_get_time();
                ESP_LOGI(TAG, "SHOT at %d,%d", tx, ty);
            } else if (s_pages.current == PAGE_CAMERA) {
                /* Any tap off the buttons navigates, so Gallery/Level/Clock are
                   reachable even when a swipe does not register. */
                page_t p = tx < c->w / 2 ? pages_back(&s_pages, now)
                                         : pages_advance(&s_pages, now);
                ESP_LOGI(TAG, "camera tap -> page %d", (int)p);
            } else if (s_pages.current == PAGE_GALLERY
                       && s_gallery_count > 0 && in_gallery_del_button(c, tx, ty)) {
                gallery_delete_current();
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "gallery delete");
            } else if (s_pages.current == PAGE_GALLERY
                       && s_gallery_count > 0 && in_rect(c, tx, ty, gallery_prev_button_rect)) {
                s_gallery_idx = (s_gallery_idx - 1 + s_gallery_count) % s_gallery_count;
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "gallery prev");
            } else if (s_pages.current == PAGE_GALLERY
                       && s_gallery_count > 0 && in_rect(c, tx, ty, gallery_next_button_rect)) {
                s_gallery_idx = (s_gallery_idx + 1) % s_gallery_count;
                s_pages.last_activity_us = now;
                s_drawn_page = PAGE_COUNT;
                ESP_LOGI(TAG, "gallery next");
            } else if (s_pages.current == PAGE_GALLERY) {
                /* A tap off the buttons pages out, so the Gallery is not a
                   trap if a swipe does not register. */
                page_t p = tx < c->w / 2 ? pages_back(&s_pages, now)
                                         : pages_advance(&s_pages, now);
                ESP_LOGI(TAG, "gallery tap -> page %d", (int)p);
#endif
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

        /*
         * A horizontal swipe pages left or right. touch_tapped() above already
         * polled the controller, so this only reads what the gesture came to;
         * a driver with no swipe (the CrowPanel's) returns 0. A swipe during
         * the saver just wakes, like a tap.
         */
        if (touch) {
            int swipe = touch_swipe();
            if (swipe != 0) {
                if (s_saver) {
                    s_pages.last_activity_us = now;
                } else {
                    page_t p = swipe > 0 ? pages_advance(&s_pages, now)
                                         : pages_back(&s_pages, now);
                    ESP_LOGI(TAG, "swipe %s -> page %d",
                             swipe > 0 ? "next" : "prev", (int)p);
                    s_auto_jumped = false;
                }
            }
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
#if CONFIG_SCREEN_ENV_ONLY
        env_sample(now);
#endif
#if CONFIG_SCREEN_BOARD_TOUCH_LCD_147
        env_sdlog(now);
#endif

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
        if (s_pages.current == PAGE_LEVEL || s_pages.current == PAGE_BUBBLE
         || s_pages.current == PAGE_PARTICLES)
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
#if HAVE_PIP
        /* Pip is a live face, always moving -- no burn-in to protect against,
           and a screensaver that replaced her with a drifting clock would be a
           downgrade. She is home and stays put. */
        if (s_pages.current == PAGE_PIP) s_pages.last_activity_us = now;
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

#if CONFIG_SCREEN_HAVE_CAMERA
        /* The camera's own transition tracking, separate from s_drawn_page
           below: that gets reset for reasons that are not a page change (a
           quiet redraw, a refresh timer), and starting or stopping the
           sensor on those would be wrong -- this only fires when
           s_pages.current itself actually changes. */
        {
            static page_t s_prev_page = PAGE_COUNT;
            if (s_pages.current != s_prev_page) {
                if (s_pages.current == PAGE_CAMERA) {
                    if (camera_start() == ESP_OK) sd_mount();
                } else if (s_prev_page == PAGE_CAMERA) {
                    camera_stop();
                }
                if (s_pages.current == PAGE_GALLERY) {
                    sd_mount();
                    gallery_scan();
                    s_gallery_idx = 0;
                }
                s_prev_page = s_pages.current;
            }
        }
#endif

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
                   merged view. The clock draws itself below, every second, and
                   does not clear as it goes -- so wipe the previous page here,
                   on entry, or it shows through beneath the clock (a room chart
                   bleeding around the digits on envio's cycling dashboard). */
                canvas_clear(c);
                switch (s_pages.current) {
                case PAGE_MESSAGE:  draw_message(c); display_blit(); break;
                case PAGE_TODAY:    views_today(c, &s_data); display_blit(); break;
                case PAGE_SETTINGS: views_settings(c, &s_settings); display_blit(); break;
                case PAGE_MENU:     views_menu(c, &s_pages, s_cycle_off, s_menu_held); display_blit(); break;
#if CONFIG_SCREEN_HAVE_CAMERA
                case PAGE_GALLERY:  draw_gallery(c); break;
#endif
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

#if CONFIG_SCREEN_ENV_ONLY
        if (s_pages.current == PAGE_ROOM_TEMP) draw_room(c, ENV_TEMP);
        if (s_pages.current == PAGE_ROOM_RH)   draw_room(c, ENV_RH);
        if (s_pages.current == PAGE_ROOM_HPA)  draw_room(c, ENV_HPA);
        if (s_pages.current == PAGE_ROOM_VOC)  draw_room(c, ENV_VOC);
        if (s_pages.current == PAGE_ROOM_CO2)  draw_room(c, ENV_CO2);
        if (s_pages.current == PAGE_CLIMATE)   draw_climate(c);
        if (s_pages.current == PAGE_AIR)       draw_air(c);
        if (s_pages.current == PAGE_TREND)     draw_trend(c);
        if (s_pages.current == PAGE_WEEK)      draw_week(c, now);
#endif
        if (s_pages.current == PAGE_CLOCK) draw_clock(c, now);
        if (s_pages.current == PAGE_FORECAST) draw_forecast(c);
        if (s_pages.current == PAGE_RTC) draw_rtc(c, now);
        if (s_pages.current == PAGE_TEMPS) {
            templog_draw(&s_templog, c);
            display_blit();
        }
#if HAVE_LEVEL
        /* The Feather's spirit-level game: PAGE_LEVEL on the boards with
           buttons and no touch, PAGE_BUBBLE at envio's touch "Level" slot.
           Same state either way (level_step, in main.c) -- a board only
           ever offers one of the two pages, so they never contend for it.
           envio alone gets her own big rendering; everyone else keeps
           level.c's small-panel look untouched. */
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
        if (s_pages.current == PAGE_BUBBLE && s_imu) draw_levelbig(c);
#else
        if ((s_pages.current == PAGE_LEVEL || s_pages.current == PAGE_BUBBLE)
            && s_imu) draw_level(c);
#endif
#endif
#if HAVE_PARTICLES
        if (s_pages.current == PAGE_PARTICLES && s_imu) draw_particles(c, now);
        if (s_pages.current == PAGE_TIMER && s_imu) draw_timer(c, now);
        if (s_pages.current == PAGE_STOPWATCH && s_imu) draw_stopwatch(c, now);
#endif
#if HAVE_PIP
        if (s_pages.current == PAGE_PIP) draw_pip(c, now);
#endif
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
        if (s_pages.current == PAGE_SYSTEM) draw_system(c);
#endif
#if CONFIG_SCREEN_HAVE_CAMERA
        /* Live, so drawn every loop like PAGE_SYSTEM/PAGE_PIP above rather
           than through the one-shot pd->draw path -- a static viewfinder
           frame would be pointless. Lifecycle (camera_start/stop) is above,
           keyed off the page transition, not here. */
        if (s_pages.current == PAGE_CAMERA) draw_camera(c);
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
                float se;
                if (drift_ppm_err(&s_drift, &ppm, &se))
                    ESP_LOGI(TAG, "board vs chip: %+d ms over %d min, %+.1f +/- %.1f ppm",
                             (int)drift_slip_ms(&s_drift),
                             drift_span_s(&s_drift) / 60, (double)ppm, (double)se);
                else
                    ESP_LOGI(TAG, "board vs chip: %+d ms, %d samples, slope not yet worth quoting",
                             (int)drift_slip_ms(&s_drift), n);
            }
            if (have_air)
                ESP_LOGI(TAG, "room %.1f C, %.0f%% RH, %.1f hPa",
                         (double)air, (double)rh, (double)env_msl(hpa));
        }
    }
}
