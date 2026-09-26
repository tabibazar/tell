#include "speaker_app.h"
#include "display.h"
#include "canvas.h"
#include "noiseui.h"

#include "ble_uart.h"
#include "ds3231.h"
#include "i2cbus.h"
#include "ring.h"
#include "sdcard.h"
#include "soundlevel.h"
#include "timecalc.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * speaker: the room's noise level, on a ring of seven LEDs and on the card.
 *
 * What runs, and where:
 *
 *   - The audio task, on core 1, reads the ES7210's four TDM slots from I2S
 *     and feeds the two mics to soundlevel.c. It also watches the third slot,
 *     the speaker's own drive looped back, which says exactly when the chime
 *     is sounding.
 *   - The LED task, at 50 Hz, turns the levels into the ring's seven colours
 *     (ring.c) and sends them down GPIO38.
 *   - The control task, every 100 ms, does everything slow: the BLE commands,
 *     the clock chip, the side keys, the card, the chime and the line on the
 *     serial console once a second, and the "days:" line once a minute.
 *   - The USB reader takes lines from the Mac's relay on the console's own USB
 *     serial port: "!" commands, queued exactly as BLE's are, and "!play",
 *     whose speech it parks in PSRAM as fast as it arrives.
 *   - The player plays that speech through the chime's amp sequence, from its
 *     own task, so a 30 s clip never holds up the control task.
 *
 * The BLE callbacks and the USB reader only queue what arrived, so the clock
 * chip, NVS and the card are only ever driven from the control task -- the
 * rule main.c keeps for the clock chip too -- and two commands never run at
 * once. The speaker itself is one at a time too: the chime and a play each
 * claim it (sound_claim) or do not sound.
 *
 * All of it is SAFE ORDER FIRST. The expander has a pin that takes USB away
 * and another that switches the amplifier on, and the ring latches whatever
 * noise is on its line at power-up, so speaker_app_main() brings the board up
 * in the order docs/hardware/speaker-pinout.md (boot_safety) sets out, and
 * the comments at each step say why.
 */

static const char *TAG = "speaker";

/* ---- the board ------------------------------------------------------------ */

#define PIN_LED         38      /* the ring's data line, straight from the chip */
#define PIN_BOOT        0       /* BOOT: 10k pull-up, low while pressed */

#define PIN_I2S_MCLK    12
#define PIN_I2S_BCLK    13
#define PIN_I2S_WS      14
#define PIN_I2S_DOUT    16      /* to the ES8311, for the speaker */
#define PIN_I2S_DIN     15      /* from the ES7210, the mics */

/*
 * The TCA9555. EXIOn is bit n. Only EXIO8 is ever made an output; everything
 * else stays the input it powers up as:
 *   - EXIO3 is the card's D3, which must read high when the card powers up
 *     (low selects SPI mode); its 10k pull-up does that as long as nothing
 *     drives it.
 *   - EXIO6 is Camera_SEL. Low moves GPIO19/20 from USB to the camera
 *     connector, and every flash after that needs BOOT held through a reset.
 *     It is named here only so the boot check can prove it is still an input.
 *   - EXIO9-11 are the side keys, which pull to ground: an output there would
 *     be shorted by a press.
 */
#define TCA_ADDR        ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000     /* 0x20 */
#define EXIO_SD_D3      IO_EXPANDER_PIN_NUM_3
#define EXIO_CAM_SEL    IO_EXPANDER_PIN_NUM_6
#define EXIO_AMP        IO_EXPANDER_PIN_NUM_8                       /* PA_CTRL: high = the NS4150B on */
#define EXIO_KEY1       IO_EXPANDER_PIN_NUM_9
#define EXIO_KEY2       IO_EXPANDER_PIN_NUM_10
#define EXIO_KEY3       IO_EXPANDER_PIN_NUM_11

/*
 * The ES7210's four channels, as they come out of one TDM frame. The
 * datasheet (Fig. 2e) sends CH1 and CH3 while LRCK is low and CH2 and CH4
 * while it is high, so slot by slot the frame is CH1, CH3, CH2, CH4:
 *   CH1 = MIC1; CH3 = the speaker's drive looped back through a -24 dB
 *   divider (the reference); CH2 = MIC2; CH4 = nothing (its inputs are capped
 *   to ground).
 * xiaozhi reads this board's mics in the same order. The factory demo's
 * "[ref, MIC1, unused, MIC2]" is the same frame read as two 32-bit slots,
 * which swaps each pair; this reads true 16-bit TDM slots, so no swap.
 * The first seconds of the log print each slot's level, so a board that
 * disagrees shows it at once: the two mics read the room, the reference and
 * the spare read near silence.
 */
enum { SLOT_MIC1 = 0, SLOT_REF = 1, SLOT_MIC2 = 2, SLOT_SPARE = 3, SLOTS = 4 };
static const char *const SLOT_NAME[SLOTS] = { "MIC1", "REF", "MIC2", "spare" };

#define READ_FRAMES     256     /* 16 ms a read; also one DMA buffer */
#define DMA_BUFFERS     6       /* 96 ms queued each way */

/* 24 dB on the mics: headroom for a loud room (the spec). The reference and
   the spare get the same, which only moves where the reference's level
   reads, and nothing is made of that but a comparison with its own floor. */
#define MIC_GAIN_DB     24.0f

/* ---- the sound ------------------------------------------------------------ */

/*
 * The chime, and the rules around the amplifier.
 *
 * The amp is on only while a chime (or a play, below) sounds. The NS4150B
 * needs 120 ms after its enable goes high before it passes sound cleanly, so
 * the codec streams silence for 150 ms first, and 60 ms of silence follow the
 * tones before the amp goes off. An enabled amp with an idle DAC hisses, which
 * is the other reason it stays off between chimes.
 *
 * Volume is esp_codec_dev's 0-100, where 60 is -20 dB. No figure for the
 * speaker's safe level exists, 60 is what the factory firmware uses, and the
 * mics sit centimetres away.
 */
#define CHIME_VOLUME    60
_Static_assert(CHIME_VOLUME <= 60, "the chime's volume is capped at 60");
#define CHIME_PEAK      0.5f    /* -6 dBFS, on the sine's peak */
#define AMP_SETTLE_MS   150     /* NS4150B start-up is 120 ms typical */
#define CHIME_TAIL_MS   60
#define CHIME_RED_S     60.0f
#define CHIME_EVERY_US  (10 * 60 * 1000000LL)

/*
 * The gate stays on until at least 150 ms of audio CAPTURED after the amp
 * went off has been fed to the meter (the NS4150B's shutdown takes 80). What
 * is waited is wall time, so it is longer: when the gate lifts, the audio
 * task can still hold samples captured up to two reads (32 ms) earlier, one
 * read in its hands and one DMA buffer done and waiting, and a 10 ms tick
 * can cut a delay short by one tick. 150 + 32 + 10 is under 200.
 */
#define GATE_HOLD_MS    150
#define GATE_WAIT_MS    200
_Static_assert(GATE_WAIT_MS >= GATE_HOLD_MS + 2 * READ_FRAMES * 1000 / SOUNDLEVEL_FS + 1000 / CONFIG_FREERTOS_HZ,
               "the gate's wait must cover 150 ms of captured audio");

/* No sound at all from 21:00 to 06:00, whatever the settings say. Fixed, not a
   setting, so nothing sent over BLE can move it. */
#define QUIET_FROM_H    21
#define QUIET_TO_H      6

/* How far ahead a chime looks for the quiet hours. From the check to the amp
   going off is about a second -- 50 ms queued, 150 of settling, 600 of tones,
   160 of tail and drain -- so a chime that passed the check at 20:59:59.5
   would still be sounding at 21:00. Five seconds covers it with room over. */
#define CHIME_AHEAD_S   5

/* ---- shared state --------------------------------------------------------- */

/* The meter, and the lock around it: the audio task feeds it, the LED and
   control tasks read it. soundlevel.c is not thread-safe by design. */
static soundlevel_t s_sl;
static SemaphoreHandle_t s_sl_lock;

static void sl_lock(void) { xSemaphoreTake(s_sl_lock, portMAX_DELAY); }
static void sl_unlock(void) { xSemaphoreGive(s_sl_lock); }

/*
 * The time, as a base and the ESP timer since. Only the control task (and
 * the boot, before it starts) writes it; the audio and LED tasks read a copy
 * taken under a spinlock. The day is carried as days since 1970, so crossing
 * midnight is arithmetic, not an event anyone has to notice.
 */
typedef struct {
    bool     have_time;     /* a time of day, from the chip or a Mac */
    bool     have_date;     /* and the date that goes with it */
    int32_t  base_day;
    uint32_t base_secs;     /* seconds since local midnight at base_us */
    int64_t  base_us;
} spk_clock_t;

static spk_clock_t s_clock;
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;

/* What NVS keeps. The LED task reads it without a lock: single bytes, written
   only by the control task. */
typedef struct {
    float   cal_offset;
    bool    calibrated;
    bool    chime;          /* off until `!chime on` */
    bool    ring;
    uint8_t night_from;     /* the ring dims from this hour ... */
    uint8_t night_to;       /* ... until this one */
} spk_settings_t;

static volatile spk_settings_t s_set = {
    .cal_offset = SOUNDLEVEL_CAL_EST_DB, .calibrated = false,
    .chime = false, .ring = true, .night_from = 22, .night_to = 7,
};

/* The chime's gate, and what the reference slot says. Written by one task and
   read by another as single words, which the S3 does atomically. */
static volatile bool  s_gated;
static volatile float s_ref_dbfs = NAN;         /* the last read's reference level */
static volatile float s_ref_floor_dbfs = NAN;   /* its level with nothing playing */
static volatile float s_ref_peak_dbfs = NAN;    /* its loudest while gated */
static volatile float s_slot_dbfs[SLOTS] = { NAN, NAN, NAN, NAN };
static volatile uint32_t s_read_errors;

/* The hardware. */
static led_strip_handle_t s_strip;
static esp_io_expander_handle_t s_io;
static bool s_amp_ok;               /* EXIO8 is ours and was proved low: the chime may use it */
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_in, s_out;
static bool s_rtc;

static ring_t s_ring;

/*
 * The speaker, one sound at a time. The chime claims it on the control task,
 * a play on the USB reader (and the player releases it when the amp is off
 * and the gate has lifted). A claim that fails is not waited for: the chime
 * tries again on the next pass, a play is refused as busy. While it is held,
 * the amp may be on from a task other than the control task, so amp_watch
 * leaves EXIO8 alone.
 */
static volatile bool s_sound_busy;
static portMUX_TYPE s_sound_mux = portMUX_INITIALIZER_UNLOCKED;

static bool sound_claim(void)
{
    bool got = false;
    portENTER_CRITICAL(&s_sound_mux);
    if (!s_sound_busy) s_sound_busy = got = true;
    portEXIT_CRITICAL(&s_sound_mux);
    return got;
}

static void sound_release(void)
{
    portENTER_CRITICAL(&s_sound_mux);
    s_sound_busy = false;
    portEXIT_CRITICAL(&s_sound_mux);
}

/* ---- the clock ------------------------------------------------------------ */

static spk_clock_t clock_get(void)
{
    spk_clock_t c;
    portENTER_CRITICAL(&s_clock_mux);
    c = s_clock;
    portEXIT_CRITICAL(&s_clock_mux);
    return c;
}

static void clock_put(const spk_clock_t *c)
{
    portENTER_CRITICAL(&s_clock_mux);
    s_clock = *c;
    portEXIT_CRITICAL(&s_clock_mux);
}

/* Seconds since the base's midnight, clamped: a base set a moment after `now`
   was read must not run the clock backwards (see timecalc_since). */
static uint64_t clock_secs(const spk_clock_t *c, int64_t now_us)
{
    int64_t us = now_us - c->base_us;
    if (us < 0) us = 0;
    return (uint64_t)c->base_secs + (uint64_t)(us / 1000000);
}

/* The local time now: false with no time at all. `day` is SOUNDLEVEL_NO_DAY
   without a date, which is how soundlevel.c is told not to place anything. */
static bool clock_now(int32_t *day, uint32_t *tod)
{
    spk_clock_t c = clock_get();
    *day = SOUNDLEVEL_NO_DAY;
    *tod = 0;
    if (!c.have_time) return false;
    uint64_t secs = clock_secs(&c, esp_timer_get_time());
    *tod = (uint32_t)(secs % SECS_PER_DAY);
    if (c.have_date) *day = c.base_day + (int32_t)(secs / SECS_PER_DAY);
    return true;
}

/*
 * The date never steps back across midnight by a little.
 *
 * Once our clock has passed midnight the meter has closed the old day and
 * the card has written its last line. Stepping the date back reopens that day
 * EMPTY -- soundlevel.c starts a fresh day on any change of date -- and the
 * next minute's daily_put, which replaces a line by its date, writes the
 * near-empty day over the finished one. The day's figures are gone, and the
 * next minutes' detail lines land in the wrong day's file.
 *
 * The step back has two ordinary causes. The Mac's `!clock` date is taken
 * before tools/push-clock.sh fetches the weather and waits for the BLE lock,
 * so it can be up to a minute old when it lands: a push started at 23:59:55
 * arrives at 00:00:03 carrying yesterday's date. And a sync that corrects a
 * clock running a few seconds fast lands just after our midnight with a time
 * just before it. So within MIDNIGHT_GUARD_S after our midnight, a date one
 * day behind ours is not taken, and a time just before midnight is held at
 * 00:00:00 of our day instead. A larger error is still corrected: the next
 * sync, five minutes later, falls outside the window.
 */
#define MIDNIGHT_GUARD_S  (5u * 60u)

/*
 * A new time of day, from a Mac's sync or the chip. It carries no date, so
 * the day it belongs to is worked out from where our own clock is: a
 * correction that crosses midnight (23:59:58 set to 00:00:03, or back) moves
 * the day with it rather than leaving the date a day out -- except the small
 * step back just after midnight, which is held at midnight (see above).
 */
static void clock_set_time(uint32_t secs, int64_t at_us)
{
    spk_clock_t c = clock_get();
    secs %= SECS_PER_DAY;
    if (c.have_time && c.have_date) {
        uint64_t was = clock_secs(&c, at_us);
        int32_t day = c.base_day + (int32_t)(was / SECS_PER_DAY);
        uint32_t tod = (uint32_t)(was % SECS_PER_DAY);
        if (tod >= 18u * 3600u && secs < 6u * 3600u) {
            day++;
        } else if (tod < MIDNIGHT_GUARD_S && secs >= SECS_PER_DAY - MIDNIGHT_GUARD_S) {
            char hms[9];
            timecalc_format_hms(secs, hms);
            ESP_LOGW(TAG, "clock: %s would step the date back across midnight; held at 00:00:00", hms);
            secs = 0;
        } else if (tod < 6u * 3600u && secs >= 18u * 3600u) {
            day--;
        }
        c.base_day = day;
    }
    c.base_secs = secs;
    c.base_us = at_us;
    c.have_time = true;
    clock_put(&c);
}

/*
 * Today's date, as days since 1970; the time of day carries on as it was.
 * False, and nothing changes, for a date one day behind ours while ours is
 * still within MIDNIGHT_GUARD_S of its midnight: a date sent just before
 * midnight (see above).
 */
static bool clock_set_date(int32_t day)
{
    spk_clock_t c = clock_get();
    int64_t now = esp_timer_get_time();
    if (c.have_time) {
        uint64_t secs = clock_secs(&c, now);
        if (c.have_date && day == c.base_day + (int32_t)(secs / SECS_PER_DAY) - 1
            && secs % SECS_PER_DAY < MIDNIGHT_GUARD_S)
            return false;
        c.base_secs = (uint32_t)(secs % SECS_PER_DAY);
        c.base_us = now;
    }
    c.base_day = day;
    c.have_date = true;
    clock_put(&c);
    return true;
}

/* Is `tod` in the window from `from` o'clock to `to` o'clock? The window may
   run across midnight; an empty one (from == to) holds nothing. */
static bool in_hours(uint32_t tod, int from, int to)
{
    int h = (int)(tod / 3600u);
    if (from == to) return false;
    if (from < to) return h >= from && h < to;
    return h >= from || h < to;
}

/* No sound: in the quiet hours now or `ahead_s` seconds from now, and also
   whenever the time is not known, since then nobody can say it is not the
   quiet hours. The window is one stretch of hours and `ahead_s` is seconds,
   so testing its two ends tests everything between. */
static bool quiet_within(uint32_t ahead_s)
{
    int32_t day;
    uint32_t tod;
    if (!clock_now(&day, &tod)) return true;
    return in_hours(tod, QUIET_FROM_H, QUIET_TO_H)
        || in_hours((tod + ahead_s) % SECS_PER_DAY, QUIET_FROM_H, QUIET_TO_H);
}

static void format_date(int32_t day, char out[11])
{
    int y, m, d;
    timecalc_civil(day, &y, &m, &d);
    snprintf(out, 11, "%04d-%02d-%02d", y, m, d);
}

/* ---- settings (NVS) ------------------------------------------------------- */

#define NVS_NS "speaker"

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "settings: none stored; the defaults");
        return;
    }
    int32_t cdb;
    uint8_t u;
    uint16_t w;
    /* The offset in hundredths of a dB: NVS has no float, and a hundredth is
       well below anything a calibration can claim. Anything implausible is
       left at the estimate rather than believed. */
    if (nvs_get_i32(h, "cal_cdb", &cdb) == ESP_OK && cdb > 0 && cdb < 25000) {
        s_set.cal_offset = (float)cdb / 100.0f;
        if (nvs_get_u8(h, "cal_ok", &u) == ESP_OK) s_set.calibrated = u != 0;
    }
    if (nvs_get_u8(h, "chime", &u) == ESP_OK) s_set.chime = u != 0;
    if (nvs_get_u8(h, "ring", &u) == ESP_OK) s_set.ring = u != 0;
    if (nvs_get_u16(h, "night", &w) == ESP_OK && (w >> 8) < 24 && (w & 0xFF) < 24) {
        s_set.night_from = (uint8_t)(w >> 8);
        s_set.night_to = (uint8_t)(w & 0xFF);
    }
    nvs_close(h);
}

static void settings_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "settings: NVS open failed: %s", esp_err_to_name(e));
        return;
    }
    e = nvs_set_i32(h, "cal_cdb", (int32_t)lrintf(s_set.cal_offset * 100.0f));
    if (e == ESP_OK) e = nvs_set_u8(h, "cal_ok", s_set.calibrated ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u8(h, "chime", s_set.chime ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u8(h, "ring", s_set.ring ? 1 : 0);
    if (e == ESP_OK) e = nvs_set_u16(h, "night", (uint16_t)(s_set.night_from << 8 | s_set.night_to));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) ESP_LOGE(TAG, "settings: not saved: %s", esp_err_to_name(e));
}

/* ---- boot step 1: the ring, dark ------------------------------------------ */

/*
 * GPIO38 floats until something drives it, and a WS2812 takes whatever edges
 * it sees as data: the ring can power up lit in random colours. So the very
 * first thing is to start RMT on the pin and send seven blacks. It needs
 * nothing else on the board -- not the I2C bus, not the expander -- which is
 * the point: it is dark even if nothing after it comes up.
 *
 * Colour order RGB: the factory firmware sets it explicitly and two community
 * builds tested it on this board. The boot sweep in the LED task shows it by
 * eye: index 0 lights red.
 */
static void ring_start(void)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED,
        .max_leds = RING_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    esp_err_t e = led_strip_new_rmt_device(&cfg, &rmt, &s_strip);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ring: RMT on GPIO%d failed: %s", PIN_LED, esp_err_to_name(e));
        s_strip = NULL;
        return;
    }
    led_strip_clear(s_strip);
    ESP_LOGI(TAG, "ring: %d LEDs on GPIO%d, cleared", RING_LEDS, PIN_LED);
}

/* ---- boot step 2: the expander, amp held off ------------------------------ */

/*
 * The TCA9555, through esp_io_expander, in the one order that never pulses
 * the amp on.
 *
 * Creating the handle resets the chip's registers the driver's way: every pin
 * an input, and every output latch HIGH. The latch does nothing to an input,
 * but the moment EXIO8 became an output it would drive the amp on. So the
 * latch is written first -- all high but EXIO8 -- through the driver's own
 * write_output_reg, which keeps its cached copy in step (a raw I2C write
 * would not, and the driver's next write would put the stale high back). Only
 * then is EXIO8 made an output, and it comes up low. No other pin's direction
 * is touched: EXIO6 in particular is never made an output at all.
 *
 * Then the chip is read back over a separate read-only handle, which cannot
 * desync anything, and the chime is allowed the amp only if the registers
 * say what they must: EXIO8 an output latched low, every other pin an input.
 * Reading the input ports is also what releases the TCA's INT#, which is tied
 * straight to 3V3 on this board and must not be left pulling against it.
 */
/* Every address that answers on the one I2C bus, once at boot: the codecs,
   expander and RTC are expected (0x18 0x20 0x40 0x51), and a screen on the
   LCD FPC adds its touch controller -- FT6336 0x38, CST816 0x15, AXS5106
   0x63, CST328 0x1A (the pinout doc). A probe only addresses; it writes
   nothing to any device. */
static void i2c_census(void)
{
    i2c_master_bus_handle_t bus = i2cbus_handle(I2CBUS_MAIN);
    char seen[128] = "";
    size_t n = 0;
    for (uint16_t a = 0x08; a < 0x78; a++)
        if (i2c_master_probe(bus, a, 20) == ESP_OK && n + 6 < sizeof seen)
            n += (size_t)snprintf(seen + n, sizeof seen - n, " 0x%02X", (unsigned)a);
    ESP_LOGI(TAG, "i2c: answering:%s", n ? seen : " none");
}

/*
 * The LCD FPC's touch controller is held by TP_RST on EXIO1, which the
 * demos pulse low and release before talking to it (pinout doc; EXIO6 is not
 * touched). EXIO1 is made an output only after the latch already holds it
 * high, so it never glitches low by accident; then 20 ms low, released, and
 * the bus counted again. The amp's bit stays low in every latch write.
 */
static void touch_wake(void)
{
    if (s_io == NULL) return;
    const uint32_t tp_rst = IO_EXPANDER_PIN_NUM_1;
    if (s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP) != ESP_OK
        || esp_io_expander_set_dir(s_io, tp_rst, IO_EXPANDER_OUTPUT) != ESP_OK) {
        ESP_LOGW(TAG, "touch: TP_RST not driven");
        return;
    }
    s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP & ~tp_rst);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP);
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "touch: TP_RST pulsed");
    i2c_census();
}

/*
 * The touch controller answered at 0x58, not the CST328's documented 0x1A.
 * Read what it says about itself through the CST328's own registers (16-bit
 * addresses, big-endian): into debug-info mode at D101, the panel size at
 * D1F4, the IC type at D204, the firmware at D208; then back to normal
 * reporting at D109. Only this chip is written to, and only its mode.
 */
static void touch_identify(void)
{
    i2c_master_bus_handle_t bus = i2cbus_handle(I2CBUS_MAIN);
    i2c_master_dev_handle_t tp;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x58,
                               .scl_speed_hz = 100000 };
    if (i2c_master_probe(bus, 0x58, 20) != ESP_OK || i2c_master_bus_add_device(bus, &dc, &tp) != ESP_OK) {
        ESP_LOGW(TAG, "touch: nothing at 0x58");
        return;
    }
    uint8_t sz[4] = { 0 }, ic[4] = { 0 }, fw[4] = { 0 };
    esp_err_t e0 = i2c_master_transmit(tp, (const uint8_t[]){ 0xD1, 0x01 }, 2, 50);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_err_t e1 = i2c_master_transmit_receive(tp, (const uint8_t[]){ 0xD1, 0xF4 }, 2, sz, 4, 50);
    esp_err_t e2 = i2c_master_transmit_receive(tp, (const uint8_t[]){ 0xD2, 0x04 }, 2, ic, 4, 50);
    esp_err_t e3 = i2c_master_transmit_receive(tp, (const uint8_t[]){ 0xD2, 0x08 }, 2, fw, 4, 50);
    i2c_master_transmit(tp, (const uint8_t[]){ 0xD1, 0x09 }, 2, 50);
    ESP_LOGI(TAG, "touch 0x58: mode %s; D1F4 %02X %02X %02X %02X (%s); D204 %02X %02X %02X %02X (%s); "
             "D208 %02X %02X %02X %02X (%s)", esp_err_to_name(e0),
             sz[0], sz[1], sz[2], sz[3], esp_err_to_name(e1), ic[0], ic[1], ic[2], ic[3], esp_err_to_name(e2),
             fw[0], fw[1], fw[2], fw[3], esp_err_to_name(e3));
    i2c_master_bus_rm_device(tp);
}

/*
 * speaker's own screen: watch's Sound page (noiseui.c), fed straight from the
 * meter rather than over a relay. Once a second: the fast level, the 3 s
 * LAeq, today's, and the hour's strip of 10 s energy means. Drawn into the
 * middle 280 rows of the 240x320 panel, the page's own size.
 */
static noiseui_t s_scr;
static bool s_screen_ok;     /* the panel came up */

static void screen_task(void *arg)
{
    (void)arg;
    canvas_t *c = display_canvas();
    canvas_t page = *c;
    int top = (c->h - NOISEUI_HEIGHT) / 2;
    page.fb = c->fb + (size_t)top * (size_t)c->w;
    page.h = NOISEUI_HEIGHT;
    for (int i = 0; i < NOISEUI_POINTS; i++) { s_scr.hist[i] = NAN; s_scr.hist_valid[i] = false; }
    double bucket_e = 0.0;
    int bucket_n = 0, ticks = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        soundlevel_now_t n;
        soundlevel_report_t t;
        sl_lock();
        soundlevel_now(&s_sl, &n);
        bool have_today = soundlevel_today(&s_sl, &t) && t.blocks > 0;
        sl_unlock();
        s_scr.have_signal = n.have;
        s_scr.laf = n.laf;
        s_scr.laeq3 = n.laeq3s;
        s_scr.today = have_today ? t.laeq : NAN;
        s_scr.calibrated = n.calibrated;
        if (n.have && isfinite(n.laeq3s)) {
            bucket_e += pow(10.0, n.laeq3s / 10.0);
            bucket_n++;
        }
        if (++ticks >= 10) {
            memmove(s_scr.hist, s_scr.hist + 1, (NOISEUI_POINTS - 1) * sizeof s_scr.hist[0]);
            memmove(s_scr.hist_valid, s_scr.hist_valid + 1, (NOISEUI_POINTS - 1) * sizeof s_scr.hist_valid[0]);
            bool ok = bucket_n > 0;
            s_scr.hist[NOISEUI_POINTS - 1] = ok ? (float)(10.0 * log10(bucket_e / bucket_n)) : NAN;
            s_scr.hist_valid[NOISEUI_POINTS - 1] = ok;
            bucket_e = 0.0;
            bucket_n = 0;
            ticks = 0;
        }
        canvas_clear(c);
        noiseui_draw(&page, &s_scr);
        display_blit();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
    }
}

/*
 * The 2.8" module on the LCD FPC: its reset is EXIO0, made an output only
 * once the latch holds it high, pulsed low 20 ms, then 120 ms for the ST7789
 * to come out of reset. Then a test card: which corner is which, and the
 * colours drawn as the canvas holds them (row 1) and byte-swapped (row 2) --
 * the row that reads red, green, blue, amber, grey says the byte order.
 */
static void screen_start(void)
{
    touch_identify();
    if (s_io == NULL) return;
    const uint32_t lcd_rst = IO_EXPANDER_PIN_NUM_0;
    if (s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP) != ESP_OK
        || esp_io_expander_set_dir(s_io, lcd_rst, IO_EXPANDER_OUTPUT) != ESP_OK) {
        ESP_LOGW(TAG, "screen: LCD_RST not driven; no screen");
        return;
    }
    s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP & ~lcd_rst);
    vTaskDelay(pdMS_TO_TICKS(20));
    s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "screen: ST7789 did not start");
        return;
    }
    display_set_brightness(40);            /* no need for full glare, or its heat */
    s_screen_ok = true;          /* its task starts once the meter and its lock exist */
}

static void expander_start(void)
{
    i2c_master_bus_handle_t bus = i2cbus_handle(I2CBUS_MAIN);
    esp_err_t e = esp_io_expander_new_i2c_tca95xx_16bit(bus, TCA_ADDR, &s_io);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "expander: TCA9555 at 0x%02X not found (%s); no chime, no keys",
                 (unsigned)TCA_ADDR, esp_err_to_name(e));
        s_io = NULL;
        return;
    }
    e = s_io->write_output_reg(s_io, 0xFFFF & ~(uint32_t)EXIO_AMP);
    if (e != ESP_OK) {
        /* The latch may still be high: making EXIO8 an output now could turn
           the amp on. Leave every pin an input, which is off (R31 pulls it
           down), and never touch it again. */
        ESP_LOGE(TAG, "expander: latch write failed (%s); EXIO8 left an input, no chime",
                 esp_err_to_name(e));
        return;
    }
    e = esp_io_expander_set_dir(s_io, EXIO_AMP, IO_EXPANDER_OUTPUT);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "expander: EXIO8 direction failed (%s); no chime", esp_err_to_name(e));
        return;
    }

    /* The proof, straight from the chip: input 00h-01h, output 02h-03h,
       configuration 06h-07h (1 = input). Port 0 is EXIO0-7, port 1 EXIO8-15. */
    i2c_master_dev_handle_t peek;
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA_ADDR,
        .scl_speed_hz = 100000,
    };
    uint8_t in[2] = { 0 }, out[2] = { 0 }, cfg[2] = { 0 };
    bool read = i2c_master_bus_add_device(bus, &dev, &peek) == ESP_OK;
    if (read) {
        read = i2c_master_transmit_receive(peek, (const uint8_t[]){ 0x00 }, 1, in, 2, 50) == ESP_OK
            && i2c_master_transmit_receive(peek, (const uint8_t[]){ 0x02 }, 1, out, 2, 50) == ESP_OK
            && i2c_master_transmit_receive(peek, (const uint8_t[]){ 0x06 }, 1, cfg, 2, 50) == ESP_OK;
        i2c_master_bus_rm_device(peek);
    }
    if (!read) {
        ESP_LOGE(TAG, "expander: read-back failed; the amp is not trusted, no chime");
        return;
    }
    const uint8_t amp = (uint8_t)(EXIO_AMP >> 8), cam = (uint8_t)EXIO_CAM_SEL, d3 = (uint8_t)EXIO_SD_D3;
    bool amp_low = (out[1] & amp) == 0;
    bool amp_out = (cfg[1] & amp) == 0;
    bool others_in = cfg[0] == 0xFF && (cfg[1] | amp) == 0xFF;
    bool cam_in = (cfg[0] & cam) != 0;
    ESP_LOGI(TAG, "expander: in %02X%02X out %02X%02X cfg %02X%02X (port 1, port 0)",
             (unsigned)in[1], (unsigned)in[0], (unsigned)out[1], (unsigned)out[0],
             (unsigned)cfg[1], (unsigned)cfg[0]);
    ESP_LOGI(TAG, "expander: EXIO8 (amp) %s %s; EXIO6 (Camera_SEL) %s, reads %s; EXIO3 (SD_D3) reads %s",
             amp_out ? "output" : "INPUT?", amp_low ? "LOW" : "HIGH?",
             cam_in ? "untouched input" : "OUTPUT?", (in[0] & cam) ? "high" : "LOW",
             (in[0] & d3) ? "high" : "LOW");
    if (!(amp_low && amp_out && others_in && cam_in)) {
        ESP_LOGE(TAG, "expander: not in the safe state; the chime will never turn the amp on");
        return;
    }
    s_amp_ok = true;
}

/*
 * The amp, switched by writing the whole output latch through the driver's
 * write_output_reg -- the call the boot sequence uses -- and never through
 * esp_io_expander_set_level. set_level writes only when the driver's cached
 * latch differs from what it is asked for, and the cache is updated only
 * after a write that reported success. So an "on" that reported an error but
 * reached the chip leaves the cache saying off while EXIO8 is high, and the
 * "off" after it finds nothing to change, writes nothing, returns ESP_OK and
 * logs nothing: the amp stays on, hissing, for as long as nothing else writes
 * the latch. write_output_reg always writes, and one that succeeds puts the
 * cache back in step. Only EXIO8 is an output, so the other fifteen latch
 * bits do nothing; they stay high, as the boot left them.
 *
 * Off may be written whenever the expander is there: it is the latch the
 * boot wanted, and writing a latch changes no pin's direction. On only when
 * the boot proved the expander safe (s_amp_ok). True when a write succeeded.
 */
#define AMP_TRIES       3

static bool amp_write(bool on)
{
    if (!s_io || (on && !s_amp_ok)) return false;
    uint32_t latch = on ? 0xFFFF : 0xFFFF & ~(uint32_t)EXIO_AMP;
    esp_err_t e = ESP_FAIL;
    for (int i = 0; i < AMP_TRIES && e != ESP_OK; i++) e = s_io->write_output_reg(s_io, latch);
    if (e != ESP_OK)
        ESP_LOGE(TAG, "amp: EXIO8 %s failed %d times: %s", on ? "high" : "low", AMP_TRIES, esp_err_to_name(e));
    return e == ESP_OK;
}

/*
 * The amp off, and proved off: the latch written, then EXIO8 read back from
 * the chip's input register, which gives the pin's own level whether it is an
 * input or an output. get_level reads that register over I2C every time; only
 * the output and direction registers are cached. If the pin will not read
 * low here, the watch in keys_poll goes on trying every 100 ms.
 */
static void amp_off(void)
{
    if (!s_io) return;
    for (int i = 0; i < AMP_TRIES; i++) {
        uint32_t lv = EXIO_AMP;
        if (amp_write(false) && esp_io_expander_get_level(s_io, EXIO_AMP, &lv) == ESP_OK && lv == 0) return;
    }
    ESP_LOGE(TAG, "amp: EXIO8 not proved low after the chime; the key poll keeps at it");
}

/*
 * The watch over EXIO8, from keys_poll's read of all sixteen inputs. The
 * chime runs start to finish inside one pass of the control task, the same
 * task that polls the keys, and keys_poll does not call this while a play
 * holds the speaker (s_sound_busy), so whenever this runs nothing is playing
 * and the pin must read low. If it does not, the latch is written low again,
 * and the log says so: once, then once a second for as long as it stays high.
 */
static uint32_t s_amp_high;

static void amp_watch(uint32_t inputs)
{
    if (!(inputs & EXIO_AMP)) {
        s_amp_high = 0;
        return;
    }
    if (s_amp_high++ % 10 == 0)
        ESP_LOGE(TAG, "amp: EXIO8 reads HIGH with no chime playing (%u polls); forcing it low",
                 (unsigned)s_amp_high);
    amp_write(false);
}

/* ---- boot step 3: the codecs ---------------------------------------------- */

/*
 * I2S, full duplex on one port: the ESP32 is master and both codecs slaves on
 * one set of clocks (MCLK 12, BCLK 13, WS 14), so capture and playback run at
 * one rate, 16 kHz, the rate soundlevel.c's A-weighting is designed for.
 * MCLK is 256 x fs, which is the only clocking the ES7210 uses as a slave.
 *
 * The frame is four 16-bit slots for the ES7210's TDM, 64 bits, with WS
 * changing at its middle. TX is the clock master in full duplex, so it has to
 * make that frame too: stereo, 32-bit slots, 16-bit data. The ES8311 takes
 * its 16 bits from the top of the left slot, and the chime is written to both
 * sides.
 *
 * Both channels are enabled here, before either codec is spoken to, so MCLK
 * is running when their registers are written: the one report of the ES7210
 * failing to come up after a cold boot looked like an init that never landed.
 * esp_codec_dev reconfigures the slots again when the codecs are opened and
 * lands on the same frame; the I2S_IF lines in the log show it (TX data_bit
 * 16 slot_bit 32, RX total_slot 4 slot_bit 16, both ws_width 32).
 *
 * TX clears each DMA buffer once sent, so when the chime runs out the codec
 * is fed silence, not the chime's last 16 ms over and over.
 */
static bool i2s_start(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = DMA_BUFFERS;
    chan.dma_frame_num = READ_FRAMES;
    chan.auto_clear_after_cb = true;
    esp_err_t e = i2s_new_channel(&chan, &s_tx, &s_rx);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "i2s: channels failed: %s", esp_err_to_name(e));
        return false;
    }

    i2s_std_config_t tx = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SOUNDLEVEL_FS),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
        },
    };
    tx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    tx.slot_cfg.ws_width = 32;

    i2s_tdm_config_t rx = {
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(SOUNDLEVEL_FS),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED, .din = PIN_I2S_DIN,
        },
    };
    rx.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
    rx.slot_cfg.total_slot = SLOTS;
    rx.slot_cfg.ws_width = 32;
    rx.slot_cfg.left_align = true;

    if ((e = i2s_channel_init_std_mode(s_tx, &tx)) != ESP_OK
        || (e = i2s_channel_init_tdm_mode(s_rx, &rx)) != ESP_OK
        || (e = i2s_channel_enable(s_tx)) != ESP_OK
        || (e = i2s_channel_enable(s_rx)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s: init failed: %s", esp_err_to_name(e));
        return false;
    }
    ESP_LOGI(TAG, "i2s: full duplex at %d Hz, MCLK 256 fs, running", SOUNDLEVEL_FS);
    return true;
}

/* The ES7210's registers after open: 00h is 41h once its reset is released,
   02h is C1h, the 256 x fs clocking. The cold-boot failure left both at their
   power-on values. */
static bool es7210_landed(void)
{
    int r00 = -1, r02 = -1;
    esp_codec_dev_read_reg(s_in, 0x00, &r00);
    esp_codec_dev_read_reg(s_in, 0x02, &r02);
    ESP_LOGI(TAG, "ES7210: reg00 %02X reg02 %02X (want 41 C1)", (unsigned)(r00 & 0xFF), (unsigned)(r02 & 0xFF));
    return r00 == 0x41 && r02 == 0xC1;
}

/*
 * The mics' gain goes on AFTER the open, because the open undoes anything set
 * before it: the ES7210's enable rewrites every PGA from a gain nothing sets,
 * and esp_codec_dev then applies its own stored input gain (0 dB until
 * set_in_gain is called). set_in_gain also stores 24 dB, so a re-open keeps it.
 */
static bool es7210_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = SLOTS,
        .channel_mask = 0,          /* all four slots */
        .sample_rate = SOUNDLEVEL_FS,
        .mclk_multiple = 256,
    };
    if (esp_codec_dev_open(s_in, &fs) != ESP_CODEC_DEV_OK) return false;
    esp_codec_dev_set_in_gain(s_in, MIC_GAIN_DB);
    int g1 = -1, g2 = -1;
    esp_codec_dev_read_reg(s_in, 0x43, &g1);
    esp_codec_dev_read_reg(s_in, 0x44, &g2);
    ESP_LOGI(TAG, "ES7210: PGA MIC1 %02X MIC2 %02X (want 18: on, 24 dB)",
             (unsigned)(g1 & 0xFF), (unsigned)(g2 & 0xFF));
    return true;
}

static void codecs_start(void)
{
    if (!i2s_start()) return;

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    /* The addresses are esp_codec_dev's 8-bit form (0x30, 0x80); its I2C
       layer shifts them to the chips' 0x18 and 0x40. Both share i2cbus's bus. */
    audio_codec_i2c_cfg_t c8311 = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2cbus_handle(I2CBUS_MAIN),
    };
    audio_codec_i2c_cfg_t c7210 = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = i2cbus_handle(I2CBUS_MAIN),
    };
    const audio_codec_ctrl_if_t *ctrl8311 = audio_codec_new_i2c_ctrl(&c8311);
    const audio_codec_ctrl_if_t *ctrl7210 = audio_codec_new_i2c_ctrl(&c7210);
    if (!data_if || !ctrl8311 || !ctrl7210) {
        ESP_LOGE(TAG, "codecs: interfaces failed");
        return;
    }

    /* The ES8311: the DAC only, clocked from MCLK (with use_mclk false it
       would multiply BCLK by 8, which is 512 x fs with this frame). No PA pin
       and no GPIO interface, so the codec driver cannot touch a pin: the amp
       is EXIO8's alone. */
    es8311_codec_cfg_t e8311 = {
        .ctrl_if = ctrl8311,
        .gpio_if = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .master_mode = false,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
    };
    const audio_codec_if_t *if8311 = es8311_codec_new(&e8311);

    /* The ES7210: a slave, all four inputs selected. Three or more is what
       turns its TDM output on, and its SDOUT2 is not connected, so TDM on
       SDOUT1 is the only way to get more than two channels out. */
    es7210_codec_cfg_t e7210 = {
        .ctrl_if = ctrl7210,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *if7210 = es7210_codec_new(&e7210);
    if (!if8311 || !if7210)
        ESP_LOGE(TAG, "codecs: %s%s did not answer", if8311 ? "" : "ES8311 ", if7210 ? "" : "ES7210");

    if (if8311) {
        esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = if8311, .data_if = data_if };
        s_out = esp_codec_dev_new(&cfg);
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16, .channel = 2, .channel_mask = 0,
            .sample_rate = SOUNDLEVEL_FS, .mclk_multiple = 256,
        };
        if (!s_out || esp_codec_dev_open(s_out, &fs) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "codecs: ES8311 open failed; no chime");
            s_out = NULL;
        } else {
            esp_codec_dev_set_out_vol(s_out, CHIME_VOLUME);
            ESP_LOGI(TAG, "ES8311: DAC open, volume %d", CHIME_VOLUME);
        }
    }

    if (if7210) {
        esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = if7210, .data_if = data_if };
        s_in = esp_codec_dev_new(&cfg);
        if (!s_in || !es7210_open()) {
            ESP_LOGE(TAG, "codecs: ES7210 open failed; nothing to measure");
            s_in = NULL;
            return;
        }
        /* One retry, as the cold-boot report suggests: close and open again. */
        if (!es7210_landed()) {
            ESP_LOGW(TAG, "ES7210: registers did not land; opening it again");
            esp_codec_dev_close(s_in);
            if (!es7210_open() || !es7210_landed())
                ESP_LOGE(TAG, "ES7210: still not right; the levels may be nonsense");
        }
    }
}

/* ---- the audio task ------------------------------------------------------- */

/* dBFS on AES17's scale, as soundlevel.c uses: a full-scale sine is 0. */
static float dbfs_of_ms(double ms)
{
    if (!(ms > 1e-13)) ms = 1e-13;
    return (float)(10.0 * log10(ms)) + 3.0103f;
}

static void audio_task(void *arg)
{
    (void)arg;
    static int16_t buf[READ_FRAMES * SLOTS];
    double slot_e[SLOTS] = { 0 };
    uint32_t slot_n = 0;

    for (;;) {
        if (esp_codec_dev_read(s_in, buf, sizeof buf) != ESP_CODEC_DEV_OK) {
            if (s_read_errors++ % 100 == 0)
                ESP_LOGE(TAG, "audio: I2S read failed (%u so far)", (unsigned)s_read_errors);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Each slot's energy: the reference every read, for the chime's gate;
           all four once a second, for the log. */
        float e[SLOTS] = { 0 };
        for (int i = 0; i < READ_FRAMES; i++) {
            for (int k = 0; k < SLOTS; k++) {
                float x = (float)buf[i * SLOTS + k] * (1.0f / 32768.0f);
                e[k] += x * x;
            }
        }
        float ref = dbfs_of_ms(e[SLOT_REF] / READ_FRAMES);
        s_ref_dbfs = ref;
        if (s_gated) {
            if (!(ref <= s_ref_peak_dbfs)) s_ref_peak_dbfs = ref;
        } else {
            /* The floor, with nothing playing: the ES8311's idle output and
               the divider's own noise. The room does not reach this slot --
               it is wired to the speaker's drive, not to a mic -- so it
               settles in a second or two and stays put. */
            float f = s_ref_floor_dbfs;
            s_ref_floor_dbfs = isnan(f) ? ref : f + 0.02f * (ref - f);
        }
        for (int k = 0; k < SLOTS; k++) slot_e[k] += e[k];
        slot_n += READ_FRAMES;
        if (slot_n >= SOUNDLEVEL_FS) {
            for (int k = 0; k < SLOTS; k++) {
                s_slot_dbfs[k] = dbfs_of_ms(slot_e[k] / slot_n);
                slot_e[k] = 0;
            }
            slot_n = 0;
        }

        int32_t day;
        uint32_t tod;
        clock_now(&day, &tod);
        sl_lock();
        soundlevel_feed(&s_sl, buf + SLOT_MIC1, buf + SLOT_MIC2, SLOTS, READ_FRAMES, day, tod);
        sl_unlock();
    }
}

/* ---- the LED task --------------------------------------------------------- */

static void ring_show(const ring_rgb_t px[RING_LEDS])
{
    if (!s_strip) return;
    for (int i = 0; i < RING_LEDS; i++) led_strip_set_pixel(s_strip, i, px[i].r, px[i].g, px[i].b);
    led_strip_refresh(s_strip);
}

/*
 * Which LED is index 0, and whether the colours come out in the order they
 * are sent, are things only someone looking can say. So at boot the chain is
 * lit one LED at a time in chain order -- index 0 red, 1 green, 2 blue, the
 * rest a dim white -- at a sixth of full drive. Red on the first one means
 * the order is RGB; where it sits is where the fill starts (ring_cfg_t.first).
 */
static void ring_sweep(void)
{
    static const ring_rgb_t first[3] = { { 40, 0, 0 }, { 0, 40, 0 }, { 0, 0, 40 } };
    ring_rgb_t px[RING_LEDS];
    ESP_LOGI(TAG, "ring: boot sweep in chain order: index 0 red, 1 green, 2 blue, 3-6 white");
    for (int i = 0; i < RING_LEDS; i++) {
        memset(px, 0, sizeof px);
        px[i] = i < 3 ? first[i] : (ring_rgb_t){ 16, 16, 16 };
        ring_show(px);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    memset(px, 0, sizeof px);
    ring_show(px);
}

static void led_task(void *arg)
{
    (void)arg;
    ring_rgb_t px[RING_LEDS];
    ring_sweep();

    TickType_t wake = xTaskGetTickCount();
    int64_t last = esp_timer_get_time();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(20));
        int64_t now = esp_timer_get_time();
        float dt = (float)(now - last) / 1e6f;
        last = now;

        soundlevel_now_t n;
        sl_lock();
        soundlevel_now(&s_sl, &n);
        sl_unlock();

        int32_t day;
        uint32_t tod;
        bool have = clock_now(&day, &tod);
        ring_in_t in = {
            .laf_dba = n.laf,
            .laeq3_dba = n.laeq3s,
            .dt_s = dt,
            .valid = n.have && !s_gated && s_in != NULL,
            /* No clock, no night: the day ceiling is the safe side of it. */
            .night = have && in_hours(tod, s_set.night_from, s_set.night_to),
            .enabled = s_set.ring,
        };
        ring_frame(&s_ring, &in, px);
        ring_show(px);
    }
}

/* ---- BLE: what arrives, queued for the control task ----------------------- */

typedef enum { CMD_TIME, CMD_DATE, CMD_TEXT } cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    uint32_t   secs;        /* CMD_TIME */
    int64_t    at_us;
    int32_t    day;         /* CMD_DATE */
    char       text[48];    /* CMD_TEXT: the command's first line */
} cmd_t;

static QueueHandle_t s_cmds;

static void post(const cmd_t *c)
{
    if (xQueueSend(s_cmds, c, 0) != pdTRUE) ESP_LOGW(TAG, "cmd: queue full; dropped");
}

/* The Mac's sync: seconds since local midnight, written before any message.
   The timestamp is taken here, so the queue's delay does not age it. */
static void on_time(uint32_t secs)
{
    cmd_t c = { .kind = CMD_TIME, .secs = secs, .at_us = esp_timer_get_time() };
    post(&c);
}

/*
 * A message. Only two kinds mean anything here. One is the Mac's "!clock"
 * payload (tools/push-clock.sh), whose "ymd Y M D W" line is the date --
 * parsed here rather than by usagedata.c, whose pages speaker does not have.
 * The other is a "!" command. Plain text would go to a screen, and there is
 * none; ble_uart has already logged it.
 */
static void on_message(const char *text, size_t len)
{
    if (len == 0 || text[0] != '!') return;

    if (strncmp(text, "!clock", 6) == 0) {
        const char *p = strstr(text, "\nymd ");
        int y, m, d, w;
        if (p && sscanf(p + 5, "%d %d %d %d", &y, &m, &d, &w) == 4) {
            ds3231_date_t date = { y, m, d, w };
            if (ds3231_date_valid(&date)) {
                cmd_t c = { .kind = CMD_DATE, .day = timecalc_days(y, m, d) };
                post(&c);
            }
        }
        return;
    }

    cmd_t c = { .kind = CMD_TEXT };
    size_t n = strcspn(text, "\r\n");
    if (n >= sizeof c.text) n = sizeof c.text - 1;
    memcpy(c.text, text, n);
    c.text[n] = '\0';
    post(&c);
}

/* ---- the clock chip --------------------------------------------------------- */

static bool s_rtc_pending;          /* a Mac has set the time or date; copy it to the chip */
static int64_t s_rtc_checked_us;
#define RTC_RECHECK_US (3600 * 1000000LL)

/*
 * Take the chip's time, and its date if it keeps one. The two are separate
 * reads, so across midnight they could disagree by a day; a read in the last
 * or first two seconds of the day is simply not taken, and the next check an
 * hour later is.
 */
static bool rtc_take(const char *why)
{
    uint32_t secs;
    ds3231_date_t d;
    if (!s_rtc || !ds3231_read(&secs)) return false;
    if (secs < 2 || secs > SECS_PER_DAY - 3) return false;
    bool dated = ds3231_read_date(&d);
    clock_set_time(secs, esp_timer_get_time());
    bool kept = dated && !clock_set_date(timecalc_days(d.year, d.month, d.day));
    char hms[9];
    timecalc_format_hms(secs, hms);
    if (kept) ESP_LOGW(TAG, "clock %s: %s, but its date %04d-%02d-%02d is a day behind ours just after "
                            "midnight; ours is kept", why, hms, d.year, d.month, d.day);
    else if (dated) ESP_LOGI(TAG, "clock %s: %04d-%02d-%02d %s", why, d.year, d.month, d.day, hms);
    else ESP_LOGI(TAG, "clock %s: %s, but the chip has no date; the card waits for a Mac", why, hms);
    return true;
}

static void rtc_service(int64_t now)
{
    if (!s_rtc) return;
    if (s_rtc_pending) {
        /* Written from the clock as it is now, not from the sync that asked:
           the sync may be a queue's length old. */
        s_rtc_pending = false;
        s_rtc_checked_us = now;
        spk_clock_t c = clock_get();
        if (!c.have_time) return;
        uint64_t secs = clock_secs(&c, now);
        uint32_t tod = (uint32_t)(secs % SECS_PER_DAY);
        ds3231_date_t d = { 0, 0, 0, 0 };
        if (c.have_date) {
            int32_t day = c.base_day + (int32_t)(secs / SECS_PER_DAY);
            int w = timecalc_weekday(day);
            timecalc_civil(day, &d.year, &d.month, &d.day);
            d.wday = w == 0 ? 7 : w;            /* Monday first, as date +%u gives it */
        }
        if (ds3231_write(tod, c.have_date ? &d : NULL))
            ESP_LOGI(TAG, "RTC set from the Mac%s", c.have_date ? ", with the date" : "");
        return;
    }
    /* The ESP timer drifts seconds a day; the chip does not. */
    if (now - s_rtc_checked_us > RTC_RECHECK_US) {
        s_rtc_checked_us = now;
        rtc_take("re-read from the RTC");
    }
}

/* ---- the card ------------------------------------------------------------- */

#define SD_DIR          "/sdcard/speaker"
#define DAILY_PATH      SD_DIR "/daily.csv"
#define DAILY_TMP       SD_DIR "/daily.tmp"
#define TODAY_PATH      SD_DIR "/today.bin"
#define TODAY_TMP       SD_DIR "/today.tmp"
#define DETAIL_HEADER   "time,laeq1s,lafmax1s,cal\n"
#define DAILY_HEADER    "date,laeq_day_so_far,lday_07_19,levening_19_23,lnight_23_07,max,l90,red_minutes,cal\n"
#define DETAIL_BATCH    10          /* lines held before a write: one write every 10 s */
#define SD_RETRY_US     (60 * 1000000LL)
#define SD_BAD_MAX      3           /* failures in a row before the card is let go and mounted again */
#define SD_RESTORE_TRIES (2 * SD_BAD_MAX)

static bool s_sd;                   /* mounted, with speaker/ there */
static int64_t s_sd_retry_us;
static bool s_restored;             /* today.bin has been read, or found absent: once per boot */
static int s_restore_fails;
static uint32_t s_sd_errors;        /* every failure since boot, for the log's count */
static int s_sd_bad;                /* failures since the card last did what was asked */
static bool s_sd_worked;            /* the card has done something since this mount */
static bool s_sd_lost;              /* let go by sd_down, and not mounted again yet */

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* 1 when `path` is there, 0 when it is not, -1 when the card could not say. */
static int present(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

/* FAT's rename will not replace a file, so the old one goes first. If power
   fails between the two, or the rename fails after the remove worked, the new
   one is left under the temporary name, and sd_recover puts it in place. */
static bool swap_in(const char *tmp, const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) return false;
    return rename(tmp, path) == 0;
}

/*
 * A replace cut short. A temporary file with no file of the real name beside
 * it is the only copy there is, so it is put in place; one WITH the real file
 * beside it never finished writing, and goes.
 *
 * Run at every mount, and again before every replace: the next replace opens
 * the temporary name with "w", which would empty a lone temporary file -- the
 * whole of daily.csv, after a swap_in whose remove worked and whose rename did
 * not. False when that lone file could not be put in place (or the card could
 * not say whether there is one), and so must not be written over.
 */
static bool sd_recover(const char *tmp, const char *path)
{
    int t = present(tmp);
    if (t == 0) return true;
    int p = t < 0 ? -1 : present(path);
    if (p < 0) return false;
    if (p > 0) {
        remove(tmp);                /* a replace that never finished writing */
        return true;
    }
    ESP_LOGW(TAG, "sd: %s was mid-replace; finishing it", path);
    return rename(tmp, path) == 0;
}

static void sd_fail(const char *what)
{
    s_sd_bad++;
    if (s_sd_errors++ % 30 == 0)
        ESP_LOGE(TAG, "sd: %s failed: %s (%u so far)", what, strerror(errno), (unsigned)s_sd_errors);
}

static void sd_ok(void)
{
    s_sd_bad = 0;
    s_sd_worked = true;
}

/* The card as the serial line shows it: what it last did, not merely that it
   was mounted once. */
static const char *sd_state(void)
{
    if (!s_sd) return s_sd_lost ? "lost" : "none";
    if (s_sd_bad > 0) return "failing";
    return s_sd_worked ? "ok" : "mounted";
}

/*
 * The card, mounted when first seen and let go when it stops answering. With
 * no card, sd_mount fails and says so, and saying it every second would fill
 * the log; so a failure is retried once a minute, and a card pushed in later
 * is found then.
 */
static bool sd_up(int64_t now)
{
    if (s_sd) return true;
    if (now < s_sd_retry_us) return false;
    s_sd_retry_us = now + SD_RETRY_US;
    if (sd_mount() != ESP_OK) return false;
    if (mkdir(SD_DIR, 0775) != 0 && errno != EEXIST) {
        sd_fail("mkdir speaker/");
        sd_unmount();               /* so the next try starts the card over */
        return false;
    }
    sd_recover(DAILY_TMP, DAILY_PATH);
    sd_recover(TODAY_TMP, TODAY_PATH);
    s_sd = true;
    s_sd_lost = false;
    s_sd_bad = 0;
    s_sd_worked = false;
    ESP_LOGI(TAG, "sd: logging to %s", SD_DIR);
    return true;
}

/*
 * A card that stops answering is unmounted and mounted again, not written to
 * for ever. Pulling the card is the only way to read the logs -- there is no
 * Wi-Fi and BLE only listens -- and a card pushed back in is a new card to the
 * SDMMC host, which the old mount can never reach: every open fails with EIO
 * until the host starts over. So after SD_BAD_MAX failures in a row the card
 * is let go, and sd_up's retry a minute later mounts it again and finishes any
 * replace the pull cut in half.
 *
 * today.bin is NOT read again after a remount. The day in memory already
 * holds all it held and everything since, and soundlevel_restore_day adds into
 * a day counting the same date, so a second restore would count the morning
 * twice (soundlevel.h: "twice adds twice").
 */
static void sd_down(int64_t now)
{
    ESP_LOGW(TAG, "sd: %d failures in a row; unmounting, and mounting again in a minute", s_sd_bad);
    sd_unmount();
    s_sd = false;
    s_sd_lost = true;
    s_sd_retry_us = now + SD_RETRY_US;
}

/* A level for a CSV field: one decimal, or empty when there is none. */
static const char *lvl(float v, char buf[12])
{
    if (isfinite(v)) snprintf(buf, 12, "%.1f", (double)v);
    else buf[0] = '\0';
    return buf;
}

/* The detail log: one line a second, held and written ten at a time. */
static char s_detail[1024];
static size_t s_detail_len;
static int s_detail_lines;
static int32_t s_detail_day = SOUNDLEVEL_NO_DAY;

static void detail_flush(void)
{
    if (s_detail_len == 0) return;
    char date[11], path[48];
    format_date(s_detail_day, date);
    snprintf(path, sizeof path, SD_DIR "/%s.csv", date);
    bool fresh = !exists(path);
    FILE *f = fopen(path, "a");
    if (f) {
        bool ok = (!fresh || fputs(DETAIL_HEADER, f) >= 0)
               && fwrite(s_detail, 1, s_detail_len, f) == s_detail_len;
        if (fclose(f) != 0 || !ok) sd_fail("detail write");
        else sd_ok();
    } else {
        sd_fail("detail open");
    }
    /* Dropped on failure, rather than held to grow without end. */
    s_detail_len = 0;
    s_detail_lines = 0;
}

static void detail_add(const soundlevel_report_t *r, bool calibrated)
{
    if (r->day != s_detail_day) {
        detail_flush();             /* the old day's lines go to the old day's file */
        s_detail_day = r->day;
    }
    char line[64], a[12], b[12];
    int n = snprintf(line, sizeof line, "%02u:%02u:%02u,%s,%s,%s\n",
                     (unsigned)(r->tod_s / 3600), (unsigned)(r->tod_s / 60 % 60), (unsigned)(r->tod_s % 60),
                     lvl(r->laeq, a), lvl(r->lmax, b), calibrated ? "cal" : "est");
    if (n <= 0 || (size_t)n >= sizeof line) return;
    if (s_detail_len + (size_t)n > sizeof s_detail) detail_flush();
    memcpy(s_detail + s_detail_len, line, (size_t)n);
    s_detail_len += (size_t)n;
    if (++s_detail_lines >= DETAIL_BATCH) detail_flush();
}

/*
 * One day's line in daily.csv, put in place of whatever that date had. The
 * file is copied to daily.tmp with the line replaced (or added at the end),
 * then swapped in, so a power cut leaves either the old file or the new one,
 * never half of each. Past days' lines are copied as they are -- and if they
 * cannot be (daily.csv there but not readable, a lone daily.tmp that will not
 * go back in place), nothing is replaced, since a file of only today's line
 * put in its place would be every past day gone. True when the line is in.
 */
static bool daily_put(const soundlevel_report_t *r, bool calibrated)
{
    char date[11], a[12], b[12], c[12], d[12], e[12], f[12], line[160];
    format_date(r->day, date);
    snprintf(line, sizeof line, "%s,%s,%s,%s,%s,%s,%s,%.1f,%s\n", date,
             lvl(r->laeq, a), lvl(r->period[SOUNDLEVEL_DAY], b), lvl(r->period[SOUNDLEVEL_EVENING], c),
             lvl(r->period[SOUNDLEVEL_NIGHT], d), lvl(r->lmax, e), lvl(r->l90, f),
             (double)(r->red_s / 60.0f), calibrated ? "cal" : "est");

    if (!sd_recover(DAILY_TMP, DAILY_PATH)) {
        sd_fail("daily.tmp recovery");
        return false;
    }
    FILE *in = fopen(DAILY_PATH, "r");
    if (!in && errno != ENOENT) {
        sd_fail("daily.csv open");
        return false;
    }
    FILE *out = fopen(DAILY_TMP, "w");
    if (!out) {
        sd_fail("daily.tmp open");
        if (in) fclose(in);
        return false;
    }
    bool ok = fputs(DAILY_HEADER, out) >= 0;
    bool placed = false;
    if (in) {
        char row[256];
        char last = '\n';
        while (ok && fgets(row, sizeof row, in)) {
            if (strncmp(row, "date,", 5) == 0) continue;
            if (strncmp(row, date, 10) == 0 && row[10] == ',') {
                if (!placed) ok = fputs(line, out) >= 0;
                placed = true;
                last = '\n';
                continue;
            }
            ok = fputs(row, out) >= 0;
            /* A row that starts with a NUL -- the file re-saved as UTF-16, or
               sectors of zeros after a bad write -- has no last character. */
            size_t len = strlen(row);
            if (len) last = row[len - 1];
        }
        if (ferror(in)) ok = false;         /* a read that failed part way is not the end of the file */
        fclose(in);
        /* A last line cut short by a power failure still ends its line. */
        if (ok && last != '\n') ok = fputc('\n', out) != EOF;
    }
    if (ok && !placed) ok = fputs(line, out) >= 0;
    if (fclose(out) != 0 || !ok || !swap_in(DAILY_TMP, DAILY_PATH)) {
        sd_fail("daily.csv write");
        return false;
    }
    sd_ok();
    return true;
}

/* today.bin: the day's totals, so a reboot carries on the day's figures. */
static uint8_t s_save[SOUNDLEVEL_SAVE_BYTES];

static void today_save(size_t n)
{
    if (!sd_recover(TODAY_TMP, TODAY_PATH)) {
        sd_fail("today.tmp recovery");
        return;
    }
    FILE *f = fopen(TODAY_TMP, "wb");
    if (!f) {
        sd_fail("today.tmp open");
        return;
    }
    bool ok = fwrite(s_save, 1, n, f) == n;
    if (fclose(f) != 0 || !ok || !swap_in(TODAY_TMP, TODAY_PATH)) sd_fail("today.bin write");
    else sd_ok();
}

/*
 * Once per boot, as soon as there is both a date and a card: put back what
 * today.bin holds. soundlevel adds it to a day already counting the same
 * date, takes it as it is into an empty one, and refuses one from another
 * date. Nothing is saved before this has run, or the first save would
 * overwrite the file it was about to restore.
 *
 * So only two outcomes count as done: the file read to its end (then
 * restored, or refused as torn or another day's), or no such file. A card
 * that could not say -- an open failing with EIO, FAT's long-name buffer or
 * file table running out, a read that errored part way -- leaves s_restored
 * false for the next pass, with nothing saved meanwhile. After
 * SD_RESTORE_TRIES of those, across a remount, the day carries on without
 * it, so one unreadable file cannot stop the log for good.
 */
static void today_restore(void)
{
    FILE *f = fopen(TODAY_PATH, "rb");
    if (!f && errno == ENOENT) {
        s_restored = true;
        ESP_LOGI(TAG, "sd: no today.bin; the day's totals start here");
        return;
    }
    size_t n = 0;
    bool failed = !f;
    if (f) {
        n = fread(s_save, 1, sizeof s_save, f);
        failed = ferror(f) != 0;
        fclose(f);
    }
    if (failed) {
        sd_fail("today.bin read");
        if (++s_restore_fails >= SD_RESTORE_TRIES) {
            s_restored = true;
            ESP_LOGW(TAG, "sd: today.bin unreadable %d times; the day's totals start here", s_restore_fails);
        }
        return;
    }
    s_restored = true;
    sd_ok();
    soundlevel_report_t r;
    sl_lock();
    bool ok = soundlevel_restore_day(&s_sl, s_save, n);
    bool have = soundlevel_today(&s_sl, &r);
    sl_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "sd: today.bin not restored (torn, or another day's)");
    } else if (have) {
        char date[11], a[12];
        format_date(r.day, date);
        ESP_LOGI(TAG, "sd: today.bin restored: %s, %.1f min counted, LAeq %s dBA", date,
                 (double)r.blocks / (SOUNDLEVEL_BLOCKS_1S * 60.0), lvl(r.laeq, a));
    }
}

static int32_t s_logged_sec_day = SOUNDLEVEL_NO_DAY;
static uint32_t s_logged_sec_tod = UINT32_MAX;
static int32_t s_logged_min_day = SOUNDLEVEL_NO_DAY;
static uint32_t s_logged_min_tod = UINT32_MAX;
static int32_t s_logged_yday = SOUNDLEVEL_NO_DAY;
static int32_t s_tried_yday = SOUNDLEVEL_NO_DAY;

static void sd_service(int64_t now)
{
    if (!sd_up(now)) return;
    spk_clock_t c = clock_get();
    if (!c.have_date) return;                   /* no clock, nothing on the card */
    if (!s_restored) {
        today_restore();
        if (!s_restored) {                      /* the card could not say; next pass */
            if (s_sd_bad >= SD_BAD_MAX) sd_down(now);
            return;
        }
    }

    soundlevel_report_t sec, min, today, yday;
    size_t saved = 0;
    sl_lock();
    bool have_sec = soundlevel_last_second(&s_sl, &sec);
    bool have_min = soundlevel_last_minute(&s_sl, &min);
    bool have_today = soundlevel_today(&s_sl, &today);
    bool have_yday = soundlevel_yesterday(&s_sl, &yday);
    bool new_min = have_min && (min.day != s_logged_min_day || min.tod_s != s_logged_min_tod);
    if (new_min) saved = soundlevel_save_day(&s_sl, s_save, sizeof s_save);
    bool calibrated = s_sl.calibrated;
    sl_unlock();

    if (have_sec && sec.day >= 0 && (sec.day != s_logged_sec_day || sec.tod_s != s_logged_sec_tod)) {
        s_logged_sec_day = sec.day;
        s_logged_sec_tod = sec.tod_s;
        detail_add(&sec, calibrated);
    }
    /* A day that has just closed gets its last line, with its last minute in
       it, before today's is written. A line that did not go in is tried again
       once a minute, not every pass: failing ten times a second would trip
       sd_down on a card that is otherwise logging. */
    if (have_yday && yday.day >= 0 && yday.day != s_logged_yday && (yday.day != s_tried_yday || new_min)) {
        s_tried_yday = yday.day;
        if (daily_put(&yday, calibrated)) s_logged_yday = yday.day;
    }
    if (new_min) {
        s_logged_min_day = min.day;
        s_logged_min_tod = min.tod_s;
        if (have_today && today.day >= 0) daily_put(&today, calibrated);
        if (saved) today_save(saved);
    }
    if (s_sd_bad >= SD_BAD_MAX) sd_down(now);
}

/* ---- the days line -------------------------------------------------------- */

/*
 * "days: cal 2026-09-23=49.8/36.0 2026-09-24=-- 2026-09-25=52.4/38.1*": each
 * day's LAeq and L90 in dBA, oldest first, up to DAYS_BACK days ending today,
 * for the relay to hand on to watch. It goes out once a minute and straight
 * after a !cal that was taken, from the control task, which owns the card.
 *
 * Past days come from daily.csv, today from the meter (the running figures,
 * marked "*"). A row written before the first !cal says "est" and was worked
 * out with the estimated offset; once speaker is calibrated such a row has
 * (offset - estimate) added to both levels, so the whole history is on the
 * calibration as it is now. Rows that say "cal" are taken as they are, and
 * so is everything while speaker is still on the estimate. The first token
 * says which speaker is now.
 *
 * The line starts at the oldest day in the window with anything to show, and
 * a day between with nothing is "=--". No date, no line: nobody could say
 * which day is today. No card, only today.
 */
#define DAYS_BACK       35

/* The start of field `k` (0-based) of a CSV row, or NULL when it has fewer. */
static const char *csv_field(const char *row, int k)
{
    while (k-- > 0) {
        row = strchr(row, ',');
        if (!row) return NULL;
        row++;
    }
    return row;
}

/* A level from a CSV field: NAN when it is empty or not a number. */
static float csv_level(const char *f)
{
    if (!f) return NAN;
    char *end;
    float v = strtof(f, &end);
    if (end == f || (*end != ',' && *end != '\r' && *end != '\n' && *end != '\0')) return NAN;
    return isfinite(v) ? v : NAN;
}

/* A level for the days line: one decimal, or "--". */
static const char *dlvl(float v, char buf[12])
{
    if (isfinite(v)) snprintf(buf, 12, "%.1f", (double)v);
    else strcpy(buf, "--");
    return buf;
}

static bool s_days_due;                 /* a !cal was taken: send the line on this pass */

static void days_line(void)
{
    int32_t today;
    uint32_t tod;
    if (!clock_now(&today, &tod) || today < 0) return;
    const int32_t first = today - (DAYS_BACK - 1);
    float laeq[DAYS_BACK], l90[DAYS_BACK];
    bool seen[DAYS_BACK] = { false };
    for (int i = 0; i < DAYS_BACK; i++) laeq[i] = l90[i] = NAN;

    soundlevel_now_t n;
    soundlevel_report_t td, yd;
    sl_lock();
    soundlevel_now(&s_sl, &n);
    bool have_td = soundlevel_today(&s_sl, &td) && td.blocks > 0 && td.day == today;
    bool have_yd = soundlevel_yesterday(&s_sl, &yd) && yd.blocks > 0 && yd.day >= first && yd.day < today;
    sl_unlock();
    const float rebase = n.calibrated ? n.cal_offset - SOUNDLEVEL_CAL_EST_DB : 0.0f;

    FILE *f = s_sd ? fopen(DAILY_PATH, "r") : NULL;
    if (s_sd && !f && errno != ENOENT) sd_fail("daily.csv read");
    if (f) {
        char row[256];
        while (fgets(row, sizeof row, f)) {
            int y, m, d;
            if (sscanf(row, "%4d-%2d-%2d", &y, &m, &d) != 3 || row[10] != ','
                || m < 1 || m > 12 || d < 1 || d > 31) continue;
            int32_t day = timecalc_days(y, m, d);
            if (day < first || day > today) continue;
            int i = (int)(day - first);
            laeq[i] = csv_level(csv_field(row, 1));
            l90[i] = csv_level(csv_field(row, 6));
            const char *cal = csv_field(row, 8);
            if (cal && strncmp(cal, "est", 3) == 0) {
                laeq[i] += rebase;
                l90[i] += rebase;
            }
            seen[i] = true;
        }
        fclose(f);
    }
    /* Yesterday from the meter, if its line has not reached the card yet: it
       reads out with the offset as it is now, so it needs no rebasing. */
    if (have_yd && !seen[yd.day - first]) {
        laeq[yd.day - first] = yd.laeq;
        l90[yd.day - first] = yd.l90;
        seen[yd.day - first] = true;
    }
    /* Today from the meter, over whatever the card's last minute said. */
    if (have_td) {
        laeq[DAYS_BACK - 1] = td.laeq;
        l90[DAYS_BACK - 1] = td.l90;
    }

    int from = DAYS_BACK - 1;
    for (int i = 0; i < DAYS_BACK - 1; i++) {
        if (seen[i]) {
            from = i;
            break;
        }
    }
    static char line[DAYS_BACK * 24 + 8];
    size_t len = (size_t)snprintf(line, sizeof line, "%s", n.calibrated ? "cal" : "est");
    for (int i = from; i < DAYS_BACK && len < sizeof line; i++) {
        char date[11], a[12], b[12];
        format_date(first + i, date);
        const char *star = i == DAYS_BACK - 1 ? "*" : "";
        int k;
        if (!isfinite(laeq[i]) && !isfinite(l90[i]))
            k = snprintf(line + len, sizeof line - len, " %s=--%s", date, star);
        else
            k = snprintf(line + len, sizeof line - len, " %s=%s/%s%s", date, dlvl(laeq[i], a), dlvl(l90[i], b), star);
        if (k < 0) break;
        len += (size_t)k;
    }
    ESP_LOGI(TAG, "days: %s", line);
}

/* ---- the chime ------------------------------------------------------------ */

/*
 * A soft two-tone chime, generated rather than stored: E5 then C5, a falling
 * third, 0.3 s each. Each tone rises over 8 ms, decays with a 120 ms time
 * constant and fades out over its last 30 ms, as raised cosines and an
 * exponential so nothing clicks. The two never overlap, so the peak is the
 * envelope's peak, CHIME_PEAK: -6 dBFS.
 */
#define CHIME_TONE_S    0.3f
#define CHIME_FRAMES    ((int)(2 * CHIME_TONE_S * SOUNDLEVEL_FS))

static float chime_sample(int i)
{
    static const float freq[2] = { 659.26f, 523.25f };
    const float attack = 0.008f, fade = 0.030f;
    float t = (float)i / (float)SOUNDLEVEL_FS;
    int k = t < CHIME_TONE_S ? 0 : 1;
    float u = t - (float)k * CHIME_TONE_S;      /* time into this tone */
    float env = expf(-u / 0.12f);
    if (u < attack) env *= 0.5f - 0.5f * cosf((float)M_PI * u / attack);
    if (u > CHIME_TONE_S - fade) env *= 0.5f + 0.5f * cosf((float)M_PI * (u - (CHIME_TONE_S - fade)) / fade);
    return CHIME_PEAK * env * sinf(2.0f * (float)M_PI * freq[k] * u);
}

/* `frames` of the chime from frame `from`, or of silence when `from` is
   negative, written to both slots. Blocks until the DMA takes them, which is
   what paces the sequence in real time. */
static bool out_frames(int from, int frames)
{
    static int16_t buf[READ_FRAMES * 2];
    while (frames > 0) {
        int n = frames < READ_FRAMES ? frames : READ_FRAMES;
        for (int i = 0; i < n; i++) {
            int16_t v = from < 0 ? 0 : (int16_t)lrintf(chime_sample(from + i) * 32767.0f);
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        if (esp_codec_dev_write(s_out, buf, n * 2 * (int)sizeof(int16_t)) != ESP_CODEC_DEV_OK) return false;
        if (from >= 0) from += n;
        frames -= n;
    }
    return true;
}

#define MS_FRAMES(ms)   ((ms) * SOUNDLEVEL_FS / 1000)

static void gate(bool on)
{
    sl_lock();
    soundlevel_exclude(&s_sl, on);
    sl_unlock();
    s_gated = on;
}

/*
 * The sequence, from the spec and the amp's datasheet:
 *   gate on -> silence -> EXIO8 high -> 150 ms of silence -> the chime ->
 *   60 ms of silence -> EXIO8 low, proved low -> 200 ms -> gate off.
 * The DMA holds up to 96 ms of queued audio, so a write returning means its
 * samples are queued, not played. Before the amp goes off, a queue's worth of
 * extra silence is written, so by the time the last write returns the chime
 * and its 60 ms of silence have all gone out.
 *
 * The wait after the amp is fixed time (GATE_WAIT_MS), not the reference
 * slot's say-so. CH3 loops back the ES8311's output, which is BEFORE the amp,
 * and by amp-off the DAC has been fed silence for over 150 ms, so the
 * reference is already at its floor and cannot see the amp shutting down. It
 * is still logged: its peak against its floor is the proof the chime played.
 *
 * The quiet hours are checked for the whole of the sequence, not just its
 * start (CHIME_AHEAD_S), and an amp that will not switch on gets no tones.
 */
static void chime_play(void)
{
    float floor = s_ref_floor_dbfs;
    s_ref_peak_dbfs = NAN;
    gate(true);

    const char *how = "played";
    if (!out_frames(-1, MS_FRAMES(50)))
        how = "codec write failed";
    else if (quiet_within(CHIME_AHEAD_S))
        how = "not played: the quiet hours";
    else if (!amp_write(true))
        how = "not played: the amp would not switch on";
    else if (!(out_frames(-1, MS_FRAMES(AMP_SETTLE_MS))
               && out_frames(0, CHIME_FRAMES)
               && out_frames(-1, MS_FRAMES(CHIME_TAIL_MS) + DMA_BUFFERS * READ_FRAMES)))
        how = "codec write failed";
    amp_off();                          /* whatever happened above */

    vTaskDelay(pdMS_TO_TICKS(GATE_WAIT_MS));
    float peak = s_ref_peak_dbfs, after = s_ref_dbfs;
    gate(false);
    ESP_LOGI(TAG, "chime: %s; reference floor %.1f, peak %.1f, %.1f at release, %d ms after amp off",
             how, (double)floor, (double)peak, (double)after, GATE_WAIT_MS);
}

static int64_t s_chime_us;              /* the last one; 0 before any */

static void chime_service(int64_t now)
{
    if (!s_set.chime || !s_amp_ok || !s_out || !s_in) return;
    if (s_chime_us != 0 && now - s_chime_us < CHIME_EVERY_US) return;
    if (quiet_within(CHIME_AHEAD_S)) return;
    soundlevel_now_t n;
    sl_lock();
    soundlevel_now(&s_sl, &n);
    sl_unlock();
    if (!n.have || n.red_run_s < CHIME_RED_S) return;
    if (!sound_claim()) return;         /* a play has the speaker; the next pass tries again */
    s_chime_us = now;
    ESP_LOGI(TAG, "chime: red for %.0f s (LAeq3 %.1f dBA)", (double)n.red_run_s, (double)n.laeq3s);
    chime_play();
    sound_release();
}

/* ---- the keys ------------------------------------------------------------- */

/* Presses are logged and nothing else, until Reza approves a mapping. */
static const struct { uint32_t pin; const char *name; } KEYS[] = {
    { EXIO_KEY1, "Key1 (EXIO9)" },
    { EXIO_KEY2, "Key2 (EXIO10)" },
    { EXIO_KEY3, "Key3 (EXIO11)" },
};
#define NKEYS (sizeof KEYS / sizeof KEYS[0])

static uint32_t s_keys_down;
static bool s_boot_down;
static int64_t s_key_since[NKEYS + 1];
static uint32_t s_key_errors;

static void key_edge(int k, const char *name, bool down, int64_t now)
{
    if (down) {
        s_key_since[k] = now;
        ESP_LOGI(TAG, "key: %s down", name);
    } else {
        ESP_LOGI(TAG, "key: %s up after %d ms (unassigned)", name, (int)((now - s_key_since[k]) / 1000));
    }
}

/*
 * The keys are behind the expander, and its INT# reaches no GPIO, so they are
 * polled. Reading all sixteen inputs at once also clears INT#, as every read
 * of the input ports does, which the pinout doc asks to be done routinely:
 * RTC_INT on EXIO4 changing would otherwise leave it asserted. The same read
 * carries EXIO8's level, which the amp's watch checks.
 */
static void keys_poll(int64_t now)
{
    if (s_io) {
        /* A play switches the amp from its own task, so the watch holds off
           unless the speaker was free both before the read and after it: a
           play cannot start and finish inside one I2C read. */
        bool free_before = !s_sound_busy;
        uint32_t lv = 0;
        if (esp_io_expander_get_level(s_io, 0xFFFF, &lv) == ESP_OK) {
            if (free_before && !s_sound_busy) amp_watch(lv);
            for (size_t k = 0; k < NKEYS; k++) {
                bool down = (lv & KEYS[k].pin) == 0;      /* active low */
                bool was = (s_keys_down & KEYS[k].pin) != 0;
                if (down == was) continue;
                s_keys_down ^= KEYS[k].pin;
                key_edge((int)k, KEYS[k].name, down, now);
            }
        } else if (s_key_errors++ % 100 == 0) {
            ESP_LOGW(TAG, "key: expander read failed (%u so far)", (unsigned)s_key_errors);
        }
    }
    bool boot = gpio_get_level(PIN_BOOT) == 0;
    if (boot != s_boot_down) {
        s_boot_down = boot;
        key_edge((int)NKEYS, "BOOT (GPIO0)", boot, now);
    }
}

/* ---- the serial line and !status ------------------------------------------ */

static void noise_line(void)
{
    soundlevel_now_t n;
    sl_lock();
    soundlevel_now(&s_sl, &n);
    sl_unlock();
    int32_t day;
    uint32_t tod;
    char when[9] = "--:--:--";
    bool have = clock_now(&day, &tod);
    if (have) timecalc_format_hms(tod, when);
    char a[12], b[12], c[12], t[12];
    /* Today's running LAeq since midnight, for watch's Sound page (the Mac
       relays this line); "--" until the day has a reading. */
    soundlevel_report_t today;
    sl_lock();
    bool have_today = soundlevel_today(&s_sl, &today) && today.blocks > 0;
    sl_unlock();
    ESP_LOGI(TAG, "noise: LAF %s LAeq3 %s dBA (%s) LAeq1 %s | red %.0fs%s | %s%s%s ring %s chime %s sd %s today %s",
             n.have ? lvl(n.laf, a) : "--.-", n.have ? lvl(n.laeq3s, b) : "--.-",
             n.calibrated ? "cal" : "est", n.have ? lvl(n.laeq1s, c) : "--.-",
             (double)n.red_run_s, s_gated ? " GATED" : "",
             when, have && day < 0 ? " (no date)" : "",
             have && in_hours(tod, s_set.night_from, s_set.night_to) ? " night" : "",
             s_set.ring ? "on" : "off", s_set.chime ? "on" : "off", sd_state(),
             have_today ? lvl(today.laeq, t) : "--");
}

static void slots_line(void)
{
    char v[SLOTS][12];
    for (int k = 0; k < SLOTS; k++) lvl(s_slot_dbfs[k], v[k]);
    ESP_LOGI(TAG, "slots (dBFS): %s %s  %s %s  %s %s  %s %s", SLOT_NAME[0], v[0], SLOT_NAME[1], v[1],
             SLOT_NAME[2], v[2], SLOT_NAME[3], v[3]);
}

/* ble_uart has no way to answer, so the status goes to the serial log. */
static void status_log(void)
{
    soundlevel_now_t n;
    soundlevel_report_t today;
    sl_lock();
    soundlevel_now(&s_sl, &n);
    bool have_today = soundlevel_today(&s_sl, &today);
    sl_unlock();
    char a[12], b[12], c[12];
    ESP_LOGI(TAG, "status: LAF %s LAeq1 %s LAeq3 %s dBA, red run %.1f s, offset %.2f dB (%s)",
             lvl(n.laf, a), lvl(n.laeq1s, b), lvl(n.laeq3s, c), (double)n.red_run_s,
             (double)n.cal_offset, n.calibrated ? "calibrated" : "estimated");
    if (have_today) {
        char date[11];
        format_date(today.day, date);
        ESP_LOGI(TAG, "status: today %s LAeq %s max %s L90 %s, red %.1f min", date, lvl(today.laeq, a),
                 lvl(today.lmax, b), lvl(today.l90, c), (double)(today.red_s / 60.0f));
    }
    int32_t day;
    uint32_t tod;
    char when[9] = "--:--:--", date[11] = "no date";
    if (clock_now(&day, &tod)) timecalc_format_hms(tod, when);
    if (day >= 0) format_date(day, date);
    ESP_LOGI(TAG, "status: %s %s, RTC %s; ring %s, night %02d-%02d; chime %s (amp %s), quiet %02d-%02d",
             date, when, s_rtc ? "present" : "absent", s_set.ring ? "on" : "off",
             s_set.night_from, s_set.night_to, s_set.chime ? "on" : "off",
             s_amp_ok ? "ready" : "unavailable", QUIET_FROM_H, QUIET_TO_H);
    ESP_LOGI(TAG, "status: card %s (%u failures since boot); reference floor %s dBFS; I2S read errors %u",
             sd_state(), (unsigned)s_sd_errors, lvl(s_ref_floor_dbfs, a), (unsigned)s_read_errors);
    slots_line();
}

/* ---- commands ------------------------------------------------------------- */

static bool word_is(const char *s, const char *w)
{
    size_t n = strlen(w);
    return strncmp(s, w, n) == 0 && (s[n] == '\0' || s[n] == ' ');
}

static const char *arg_of(const char *s)
{
    const char *p = strchr(s, ' ');
    while (p && *p == ' ') p++;
    return p ? p : "";
}

static void command(const char *text)
{
    const char *arg = arg_of(text);

    if (word_is(text, "!cal")) {
        char *end;
        float nn = strtof(arg, &end);
        if (end == arg) {
            ESP_LOGW(TAG, "cal: say \"!cal NN\", the room's level in dBA now");
            return;
        }
        /* soundlevel_calibrate takes it only with something measured and a
           plausible room; the same test here says which it was. */
        soundlevel_now_t n;
        sl_lock();
        soundlevel_now(&s_sl, &n);
        float was = s_sl.cal_offset;
        float off = soundlevel_calibrate(&s_sl, nn);
        sl_unlock();
        if (!n.have || !(nn >= 10.0f && nn <= 130.0f)) {
            ESP_LOGW(TAG, "cal: %.1f dBA not taken (%s)", (double)nn,
                     n.have ? "outside 10-130 dBA" : "nothing measured yet");
            return;
        }
        s_set.cal_offset = off;
        s_set.calibrated = true;
        settings_save();
        ESP_LOGI(TAG, "cal: the room is %.1f dBA: offset %.2f dB (was %.2f)", (double)nn, (double)off, (double)was);
        s_days_due = true;              /* the history, on the new calibration, straight away */
        return;
    }
    if (word_is(text, "!chime") || word_is(text, "!ring")) {
        bool chime = text[1] == 'c';
        bool on;
        if (strcmp(arg, "on") == 0) on = true;
        else if (strcmp(arg, "off") == 0) on = false;
        else {
            ESP_LOGW(TAG, "%s: say on or off", chime ? "chime" : "ring");
            return;
        }
        if (chime) s_set.chime = on;
        else s_set.ring = on;
        settings_save();
        ESP_LOGI(TAG, "%s %s%s", chime ? "chime" : "ring", on ? "on" : "off",
                 chime && on ? " (never 21:00-06:00, at most once in 10 min)" : "");
        return;
    }
    if (word_is(text, "!night")) {
        int from, to;
        if (sscanf(arg, "%d-%d", &from, &to) != 2 || from < 0 || from > 23 || to < 0 || to > 23) {
            ESP_LOGW(TAG, "night: say \"!night HH-HH\", e.g. !night 22-07");
            return;
        }
        s_set.night_from = (uint8_t)from;
        s_set.night_to = (uint8_t)to;
        settings_save();
        ESP_LOGI(TAG, "night: the ring dims %02d:00-%02d:00%s", from, to, from == to ? " (never)" : "");
        return;
    }
    if (word_is(text, "!status")) {
        status_log();
        return;
    }
    ESP_LOGI(TAG, "cmd: \"%s\" means nothing here", text);
}

static void handle(const cmd_t *c)
{
    switch (c->kind) {
    case CMD_TIME:
        clock_set_time(c->secs, c->at_us);
        s_rtc_pending = true;
        break;
    case CMD_DATE: {
        char date[11];
        format_date(c->day, date);
        if (!clock_set_date(c->day)) {
            ESP_LOGW(TAG, "clock: the Mac says %s, a day behind us just after midnight: "
                          "sent before midnight, so ours is kept", date);
            break;
        }
        s_rtc_pending = true;
        ESP_LOGI(TAG, "clock: the Mac says %s", date);
        break;
    }
    case CMD_TEXT:
        command(c->text);
        break;
    }
}

/* ---- the USB serial line: commands and speech ----------------------------- */

/*
 * The Mac's relay (tools/noise-relay.py) holds speaker's USB serial port: it
 * reads the log, and writes lines back.
 *
 *   "!cal NN", "!chime on", "!status" ... -- any "!" line but !play goes to
 *   on_message, the way a BLE write does, and so into the control task's
 *   queue: one consumer, so a command from USB and one from BLE never run at
 *   once.
 *
 *   "!play <nbytes> <rate> <normal|test>\n" and straight after it exactly
 *   <nbytes> bytes of little-endian int16 mono PCM at 16000 Hz, at most 30 s.
 *   The answer is on the log: "play: started (N bytes, mode)", then "play:
 *   done" or "play: timeout"; or at once "play: refused (why)". A refused
 *   play's bytes are read and thrown away all the same, so the next line is
 *   found where it should be. "normal" is refused in the quiet hours and when
 *   the time is not known; "test" plays at any hour.
 *
 * The driver (IDF 5.5) has no flow control: its interrupt empties the USB
 * FIFO into the RX ring and drops the packet when the ring is full, and the
 * Mac writes the whole clip far faster than it plays. So the reader never
 * waits on the codec. It copies the clip into PSRAM as it comes, 960 kB of it
 * at most, and the player task plays from there at the codec's pace.
 */
#define PLAY_RATE       SOUNDLEVEL_FS                   /* the only rate: TX and RX share the clocks */
#define PLAY_BYTES_S    (PLAY_RATE * 2)                 /* mono int16 */
#define PLAY_MAX_BYTES  (30 * PLAY_BYTES_S)             /* 960000: 30 s */
#define PLAY_GAP_MS     2000u                           /* no bytes for this long: the clip is given up */
#define PLAY_SLACK_US   (10 * 1000000LL)                /* beyond its own length, the most a play may take */
#define USB_RX_RING     (12 * 1024)                     /* the driver's ring; under 16 kB, so internal RAM */
#define USB_LINE_MAX    128

static uint8_t *s_pcm;                  /* PLAY_MAX_BYTES in PSRAM; NULL if it could not be had */
static TaskHandle_t s_player;

/* One play, set by the reader before it wakes the player. The reader alone
   writes s_play_wr and s_play_arrived_ms while the clip comes in. */
static size_t s_play_total;
static bool s_play_test;
static size_t s_play_wr;                        /* bytes of the clip in s_pcm so far (atomic) */
static volatile uint32_t s_play_arrived_ms;     /* when the last of them came */

static uint32_t ms_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* How a play ended, as the log says it. Every ending starts "play: done" or
   "play: timeout", the words the relay waits for; the ones the protocol does
   not name say what went wrong in brackets after "done". */
typedef enum { PLAYED, PLAY_TIMEOUT, PLAY_QUIET, PLAY_NO_AMP, PLAY_CODEC } play_end_t;

static const char *const PLAY_END[] = {
    [PLAYED]       = "play: done",
    [PLAY_TIMEOUT] = "play: timeout",
    [PLAY_QUIET]   = "play: done (cut short: the quiet hours)",
    [PLAY_NO_AMP]  = "play: done (not heard: the amp would not switch on)",
    [PLAY_CODEC]   = "play: done (cut short: codec write failed)",
};

/*
 * The clip, from s_pcm to the codec as it arrives: each mono sample to both
 * slots, as out_frames writes the chime, READ_FRAMES at a time. The write
 * blocks until the DMA takes it, which is what paces this in real time; when
 * the bytes come slower than that, the DMA runs dry and plays silence
 * (auto_clear_after_cb) until they catch up.
 */
static play_end_t play_stream(size_t total, bool test, int64_t deadline)
{
    static int16_t st[READ_FRAMES * 2];
    size_t rd = 0;
    while (rd < total) {
        size_t wr = __atomic_load_n(&s_play_wr, __ATOMIC_ACQUIRE);
        if (esp_timer_get_time() > deadline) return PLAY_TIMEOUT;
        if (!test && quiet_within(0)) return PLAY_QUIET;
        size_t frames = (wr - rd) / 2;
        if (frames == 0) {
            if (ms_now() - s_play_arrived_ms > PLAY_GAP_MS) return PLAY_TIMEOUT;
            vTaskDelay(1);
            continue;
        }
        if (frames > READ_FRAMES) frames = READ_FRAMES;
        const uint8_t *p = s_pcm + rd;
        for (size_t i = 0; i < frames; i++) {
            int16_t v = (int16_t)((uint16_t)p[2 * i] | (uint16_t)p[2 * i + 1] << 8);
            st[2 * i] = st[2 * i + 1] = v;
        }
        if (esp_codec_dev_write(s_out, st, (int)(frames * 2 * sizeof(int16_t))) != ESP_CODEC_DEV_OK)
            return PLAY_CODEC;
        rd += frames * 2;
    }
    return PLAYED;
}

/*
 * One play, in the chime's sequence and with the chime's helpers:
 *   gate on -> silence -> EXIO8 high -> 150 ms of silence -> the clip ->
 *   60 ms of silence and a queue's worth more -> EXIO8 low, proved low ->
 *   200 ms -> gate off.
 * A timeout, the quiet hours arriving or the deadline end the clip early and
 * go the same way out, through silence to the amp off; only a codec that will
 * not take a write skips the silence, since it could not be written either.
 * The volume is the chime's, CHIME_VOLUME, set once at boot and never touched.
 */
static void play_run(void)
{
    const size_t total = s_play_total;
    const bool test = s_play_test;
    const int64_t deadline = esp_timer_get_time() + (int64_t)total * 1000000 / PLAY_BYTES_S + PLAY_SLACK_US;

    gate(true);
    play_end_t end = PLAY_CODEC;
    if (out_frames(-1, MS_FRAMES(50))) {
        if (!amp_write(true)) end = PLAY_NO_AMP;
        else if (out_frames(-1, MS_FRAMES(AMP_SETTLE_MS))) end = play_stream(total, test, deadline);
        if (end != PLAY_CODEC && !out_frames(-1, MS_FRAMES(CHIME_TAIL_MS) + DMA_BUFFERS * READ_FRAMES))
            end = PLAY_CODEC;
    }
    amp_off();                          /* whatever happened above */

    vTaskDelay(pdMS_TO_TICKS(GATE_WAIT_MS));
    gate(false);
    ESP_LOGI(TAG, "%s", PLAY_END[end]);
}

static void player_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_run();
        sound_release();                /* the reader claimed it for this play */
    }
}

/* What the reader is in the middle of: a line, or a clip's bytes (kept for
   the player, or thrown away after a refusal). Only the reader touches it. */
static size_t s_rx_left;                /* clip bytes still to come; 0 = reading lines */
static bool s_rx_keep;
static uint32_t s_rx_last_ms;

static void play_refuse(const char *why, size_t discard)
{
    ESP_LOGI(TAG, "play: refused (%s)", why);
    s_rx_left = discard;
    s_rx_keep = false;
    s_rx_last_ms = ms_now();
}

/* A token of digits only, as a number; false for anything else. */
static bool digits(const char *tok, size_t n, unsigned long *out)
{
    if (n == 0 || n > 9) return false;  /* nine digits is past anything allowed, and fits */
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)tok[i])) return false;
        v = v * 10 + (unsigned long)(tok[i] - '0');
    }
    *out = v;
    return true;
}

/*
 * "!play <nbytes> <rate> <normal|test>". A byte count that cannot be read
 * leaves nothing to throw away, so the line after it is looked for at once;
 * any other refusal throws the stated count away. The quiet hours are checked
 * over the whole clip and CHIME_AHEAD_S more, as the chime checks its second.
 */
static void play_header(const char *line)
{
    char tok[4][16];
    size_t tl[4] = { 0 };
    int nt = 0;
    const char *p = arg_of(line);
    while (*p && nt < 4) {
        size_t n = strcspn(p, " ");
        tl[nt] = n;
        snprintf(tok[nt], sizeof tok[nt], "%.*s", (int)(n < sizeof tok[nt] ? n : sizeof tok[nt] - 1), p);
        nt++;
        p += n;
        while (*p == ' ') p++;
    }
    unsigned long nbytes = 0, rate = 0;
    if (nt < 1 || !digits(tok[0], tl[0], &nbytes) || nbytes == 0) {
        play_refuse("bad header", 0);
        return;
    }
    bool test = nt >= 3 && strcmp(tok[2], "test") == 0;
    if (nt != 3 || *p || !digits(tok[1], tl[1], &rate) || rate != PLAY_RATE
        || (!test && strcmp(tok[2], "normal") != 0) || nbytes % 2 != 0) {
        play_refuse("bad header", nbytes);
        return;
    }
    if (nbytes > PLAY_MAX_BYTES) {
        play_refuse("too long", nbytes);
        return;
    }
    if (!s_pcm || !s_player) {
        play_refuse("no memory", nbytes);
        return;
    }
    if (!s_out || !s_amp_ok) {
        play_refuse("no amp", nbytes);
        return;
    }
    if (!test && quiet_within((uint32_t)((nbytes + PLAY_BYTES_S - 1) / PLAY_BYTES_S) + CHIME_AHEAD_S)) {
        play_refuse("quiet hours", nbytes);
        return;
    }
    if (!sound_claim()) {
        play_refuse("busy", nbytes);
        return;
    }
    /* The player is idle -- it releases the claim only when it is done -- so
       the buffer is free to start again. */
    s_play_total = nbytes;
    s_play_test = test;
    __atomic_store_n(&s_play_wr, 0, __ATOMIC_RELEASE);
    s_play_arrived_ms = ms_now();
    s_rx_left = nbytes;
    s_rx_keep = true;
    s_rx_last_ms = s_play_arrived_ms;
    ESP_LOGI(TAG, "play: started (%lu bytes, %s)", nbytes, test ? "test" : "normal");
    xTaskNotifyGive(s_player);
}

/* Bytes of the clip: into s_pcm for the player, or nowhere after a refusal.
   Returns how many of the `n` belonged to it. */
static size_t clip_take(const uint8_t *p, size_t n, uint32_t now_ms)
{
    size_t k = n < s_rx_left ? n : s_rx_left;
    if (s_rx_keep) {
        size_t wr = __atomic_load_n(&s_play_wr, __ATOMIC_RELAXED);
        memcpy(s_pcm + wr, p, k);
        __atomic_store_n(&s_play_wr, wr + k, __ATOMIC_RELEASE);    /* published after the copy */
        s_play_arrived_ms = now_ms;
    }
    s_rx_left -= k;
    s_rx_last_ms = now_ms;
    return k;
}

static void usb_line(const char *line, size_t len)
{
    if (word_is(line, "!play")) play_header(line);
    else if (line[0] == '!') on_message(line, len);
}

/*
 * The reader: lines end at '\n' ('\r' is dropped, so "\r\n" works too), and
 * one longer than USB_LINE_MAX is dropped whole, up to its end. After a
 * !play header the bytes are the clip's, newlines and all, until its count is
 * in; reads then ask for no more than the clip has left, so the next line is
 * never swallowed. A clip whose bytes stop for PLAY_GAP_MS is given up and the
 * reader goes back to lines; the player sees the same silence and stops.
 */
static void usb_task(void *arg)
{
    (void)arg;
    static uint8_t in[512];
    static char line[USB_LINE_MAX];
    size_t len = 0;
    bool overlong = false;
    for (;;) {
        size_t want = sizeof in;
        if (s_rx_left > 0 && s_rx_left < want) want = s_rx_left;
        int n = usb_serial_jtag_read_bytes(in, (uint32_t)want, pdMS_TO_TICKS(100));
        uint32_t now_ms = ms_now();
        if (n <= 0) {
            if (s_rx_left > 0 && now_ms - s_rx_last_ms > PLAY_GAP_MS) {
                if (!s_rx_keep)
                    ESP_LOGW(TAG, "usb: a refused clip stopped with %u bytes to come; reading lines again",
                             (unsigned)s_rx_left);
                s_rx_left = 0;
            }
            continue;
        }
        for (size_t i = 0; i < (size_t)n;) {
            if (s_rx_left > 0) {
                i += clip_take(in + i, (size_t)n - i, now_ms);
                continue;
            }
            char ch = (char)in[i++];
            if (ch == '\n') {
                if (len > 0 && !overlong) {
                    line[len] = '\0';
                    usb_line(line, len);
                }
                len = 0;
                overlong = false;
            } else if (ch == '\r') {
                continue;
            } else if (len < sizeof line - 1) {
                line[len++] = ch;
            } else {
                overlong = true;
            }
        }
    }
}

/*
 * The driver, which ESP_LOG then writes through as well (usb_serial_jtag_vfs):
 * with no host reading, it drops the log after 50 ms rather than blocking, as
 * on watch. The reader outranks the ring's task: it only copies, and a ring
 * left full loses the clip's bytes. The player sits below both; it spends its
 * time waiting on the DMA.
 */
static void usb_start(void)
{
    s_pcm = heap_caps_malloc(PLAY_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_pcm) ESP_LOGE(TAG, "play: no %d bytes of PSRAM for a clip; plays will be refused", PLAY_MAX_BYTES);
    else if (xTaskCreate(player_task, "player", 4096, NULL, 5, &s_player) != pdPASS) s_player = NULL;

    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = USB_RX_RING, .tx_buffer_size = 2048 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "usb: serial driver not installed; no relay commands, no plays");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    xTaskCreate(usb_task, "usbline", 4096, NULL, 7, NULL);
    ESP_LOGI(TAG, "usb: listening for the relay's commands and plays");
}

/* ---- the control task ----------------------------------------------------- */

static void control_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    int64_t next_line = esp_timer_get_time() + 1000000;
    int64_t next_days = esp_timer_get_time() + 5 * 1000000;    /* soon after boot, for the relay */
    int slot_lines = 5;                 /* the slot levels, for the first few seconds */

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();

        cmd_t c;
        while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) handle(&c);
        rtc_service(now);
        keys_poll(now);
        sd_service(now);
        chime_service(now);

        if (now >= next_line) {
            next_line += 1000000;
            if (next_line < now) next_line = now + 1000000;     /* after a chime */
            noise_line();
            if (slot_lines > 0 && s_in) {
                slots_line();
                slot_lines--;
            }
        }
        if (s_days_due || now >= next_days) {
            s_days_due = false;
            next_days = now + 60 * 1000000LL;
            days_line();
        }
    }
}

/* ---- boot ----------------------------------------------------------------- */

void speaker_app_main(void)
{
    ESP_LOGI(TAG, "speaker: noise monitor, no screen");

    /* 1. The ring, dark, before anything else. */
    ring_start();

    /* 2. The bus, and the expander with the amp held off. 3. The codecs,
       MCLK first. Nothing on the bus is reachable without the bus. */
    if (i2cbus_init(I2CBUS_MAIN) == ESP_OK) {
        i2c_census();
    expander_start();
    touch_wake();
    screen_start();
        codecs_start();
    } else {
        ESP_LOGE(TAG, "i2c: bus failed; no codecs, no clock, no amp");
    }

    /* The settings, then the meter with the stored offset. ble_uart_start
       brings NVS up too, but the offset is needed before the audio starts;
       nvs_flash_init a second time is a no-op. */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        e = nvs_flash_init();
    }
    if (e == ESP_OK) settings_load();
    soundlevel_init(&s_sl, s_set.cal_offset, s_set.calibrated);
    ring_init(&s_ring);
    ESP_LOGI(TAG, "settings: offset %.2f dB (%s), chime %s, ring %s, night %02d-%02d",
             (double)s_set.cal_offset, s_set.calibrated ? "cal" : "est", s_set.chime ? "on" : "off",
             s_set.ring ? "on" : "off", s_set.night_from, s_set.night_to);

    /* The clock chip: the time, and the date if it keeps one. */
    s_rtc = ds3231_init() == ESP_OK;
    s_rtc_checked_us = esp_timer_get_time();
    if (!s_rtc || !rtc_take("from the RTC")) {
        ESP_LOGI(TAG, "clock: none yet; waiting for a Mac's sync");
        /* A chip that answered but was not taken -- read in the two seconds
           either side of midnight, say -- is asked again in ten seconds
           rather than an hour, which would be an hour with no date and so no
           log. Only once: a chip with no time says so on every read. */
        if (s_rtc) s_rtc_checked_us -= RTC_RECHECK_US - 10 * 1000000LL;
    }

    gpio_config_t boot = {
        .pin_bit_mask = 1ULL << PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot);

    s_sl_lock = xSemaphoreCreateMutex();
    if (s_screen_ok) {
        xTaskCreatePinnedToCore(screen_task, "screen", 6144, NULL, 2, NULL, 0);
        ESP_LOGI(TAG, "screen: the Sound page, from speaker's own level");
    }
    s_cmds = xQueueCreate(8, sizeof(cmd_t));

    if (s_in) xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 1);
    else ESP_LOGE(TAG, "audio: no ES7210, so nothing is measured; the ring shows only its heartbeat");
    xTaskCreate(led_task, "ring", 4096, NULL, 6, NULL);
    xTaskCreate(control_task, "control", 8192, NULL, 4, NULL);

    if (ble_uart_start(on_message, on_time) != ESP_OK)
        ESP_LOGE(TAG, "ble: start failed; no clock pushes, no commands");
    usb_start();
    ESP_LOGI(TAG, "speaker: up");
}
