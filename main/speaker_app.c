#include "speaker_app.h"

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
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
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
 *     serial console once a second.
 *
 * The BLE callbacks run on NimBLE's own task and only queue what arrived, so
 * the clock chip, NVS and the card are only ever driven from the control task
 * -- the rule main.c keeps for the clock chip too.
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
 * The amp is on only while a chime plays. The NS4150B needs 120 ms after its
 * enable goes high before it passes sound cleanly, so the codec streams
 * silence for 150 ms first, and 60 ms of silence follow the tones before the
 * amp goes off. An enabled amp with an idle DAC hisses, which is the other
 * reason it stays off between chimes.
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
#define GATE_HOLD_MS    150     /* after the amp goes off; its shutdown takes 80 */
#define CHIME_RED_S     60.0f
#define CHIME_EVERY_US  (10 * 60 * 1000000LL)

/* No sound at all from 21:00 to 06:00, whatever the settings say. Fixed, not a
   setting, so nothing sent over BLE can move it. */
#define QUIET_FROM_H    21
#define QUIET_TO_H      6

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
 * A new time of day, from a Mac's sync or the chip. It carries no date, so
 * the day it belongs to is worked out from where our own clock is: a
 * correction that crosses midnight (23:59:58 set to 00:00:03, or back) moves
 * the day with it rather than leaving the date a day out.
 */
static void clock_set_time(uint32_t secs, int64_t at_us)
{
    spk_clock_t c = clock_get();
    if (c.have_time && c.have_date) {
        uint64_t was = clock_secs(&c, at_us);
        int32_t day = c.base_day + (int32_t)(was / SECS_PER_DAY);
        uint32_t tod = (uint32_t)(was % SECS_PER_DAY);
        if (tod >= 18u * 3600u && secs < 6u * 3600u) day++;
        else if (tod < 6u * 3600u && secs >= 18u * 3600u) day--;
        c.base_day = day;
    }
    c.base_secs = secs % SECS_PER_DAY;
    c.base_us = at_us;
    c.have_time = true;
    clock_put(&c);
}

/* Today's date, as days since 1970; the time of day carries on as it was. */
static void clock_set_date(int32_t day)
{
    spk_clock_t c = clock_get();
    int64_t now = esp_timer_get_time();
    if (c.have_time) {
        c.base_secs = (uint32_t)(clock_secs(&c, now) % SECS_PER_DAY);
        c.base_us = now;
    }
    c.base_day = day;
    c.have_date = true;
    clock_put(&c);
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

/* No sound: in the quiet hours, and also whenever the time is not known,
   since then nobody can say it is not the quiet hours. */
static bool quiet_now(void)
{
    int32_t day;
    uint32_t tod;
    if (!clock_now(&day, &tod)) return true;
    return in_hours(tod, QUIET_FROM_H, QUIET_TO_H);
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

/* The amp. Only the chime calls this, and only when s_amp_ok. */
static void amp_set(bool on)
{
    if (!s_io || !s_amp_ok) return;
    esp_err_t e = esp_io_expander_set_level(s_io, EXIO_AMP, on ? 1 : 0);
    if (e != ESP_OK) ESP_LOGE(TAG, "amp: EXIO8 %s failed: %s", on ? "high" : "low", esp_err_to_name(e));
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
    if (xQueueSend(s_cmds, c, 0) != pdTRUE) ESP_LOGW(TAG, "ble: command queue full; dropped");
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
    if (dated) clock_set_date(timecalc_days(d.year, d.month, d.day));
    char hms[9];
    timecalc_format_hms(secs, hms);
    if (dated) ESP_LOGI(TAG, "clock %s: %04d-%02d-%02d %s", why, d.year, d.month, d.day, hms);
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

static bool s_sd;                   /* mounted, with speaker/ there */
static int64_t s_sd_retry_us;
static bool s_restored;             /* today.bin has been looked at, once */
static uint32_t s_sd_errors;

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* FAT's rename will not replace a file, so the old one goes first. If power
   fails between the two, the new one is left under the temporary name, and
   sd_recover puts it in place at the next mount. */
static bool swap_in(const char *tmp, const char *path)
{
    if (remove(path) != 0 && errno != ENOENT) return false;
    return rename(tmp, path) == 0;
}

static void sd_recover(const char *tmp, const char *path)
{
    if (!exists(tmp)) return;
    if (!exists(path)) {
        ESP_LOGW(TAG, "sd: %s was mid-replace; finishing it", path);
        rename(tmp, path);
    } else {
        remove(tmp);                /* a replace that never finished writing */
    }
}

static void sd_fail(const char *what)
{
    if (s_sd_errors++ % 30 == 0)
        ESP_LOGE(TAG, "sd: %s failed: %s (%u so far)", what, strerror(errno), (unsigned)s_sd_errors);
}

/*
 * The card, mounted once and then left. With no card, sd_mount fails and
 * says so, and saying it every second would fill the log; so a failure is
 * retried once a minute, and a card pushed in later is found then.
 */
static bool sd_up(int64_t now)
{
    if (s_sd) return true;
    if (now < s_sd_retry_us) return false;
    s_sd_retry_us = now + SD_RETRY_US;
    if (sd_mount() != ESP_OK) return false;
    if (mkdir(SD_DIR, 0775) != 0 && errno != EEXIST) {
        sd_fail("mkdir speaker/");
        return false;
    }
    sd_recover(DAILY_TMP, DAILY_PATH);
    sd_recover(TODAY_TMP, TODAY_PATH);
    s_sd = true;
    ESP_LOGI(TAG, "sd: logging to %s", SD_DIR);
    return true;
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
 * never half of each. Past days' lines are copied as they are.
 */
static void daily_put(const soundlevel_report_t *r, bool calibrated)
{
    char date[11], a[12], b[12], c[12], d[12], e[12], f[12], line[160];
    format_date(r->day, date);
    snprintf(line, sizeof line, "%s,%s,%s,%s,%s,%s,%s,%.1f,%s\n", date,
             lvl(r->laeq, a), lvl(r->period[SOUNDLEVEL_DAY], b), lvl(r->period[SOUNDLEVEL_EVENING], c),
             lvl(r->period[SOUNDLEVEL_NIGHT], d), lvl(r->lmax, e), lvl(r->l90, f),
             (double)(r->red_s / 60.0f), calibrated ? "cal" : "est");

    FILE *out = fopen(DAILY_TMP, "w");
    if (!out) {
        sd_fail("daily.tmp open");
        return;
    }
    bool ok = fputs(DAILY_HEADER, out) >= 0;
    bool placed = false;
    FILE *in = fopen(DAILY_PATH, "r");
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
            last = row[strlen(row) - 1];
        }
        fclose(in);
        /* A last line cut short by a power failure still ends its line. */
        if (ok && last != '\n') ok = fputc('\n', out) != EOF;
    }
    if (ok && !placed) ok = fputs(line, out) >= 0;
    if (fclose(out) != 0 || !ok || !swap_in(DAILY_TMP, DAILY_PATH)) sd_fail("daily.csv write");
}

/* today.bin: the day's totals, so a reboot carries on the day's figures. */
static uint8_t s_save[SOUNDLEVEL_SAVE_BYTES];

static void today_save(size_t n)
{
    FILE *f = fopen(TODAY_TMP, "wb");
    if (!f) {
        sd_fail("today.tmp open");
        return;
    }
    bool ok = fwrite(s_save, 1, n, f) == n;
    if (fclose(f) != 0 || !ok || !swap_in(TODAY_TMP, TODAY_PATH)) sd_fail("today.bin write");
}

/*
 * Once per boot, as soon as there is both a date and a card: put back what
 * today.bin holds. soundlevel adds it to a day already counting the same
 * date, takes it as it is into an empty one, and refuses one from another
 * date. Nothing is saved before this has run, or the first save would
 * overwrite the file it was about to restore.
 */
static void today_restore(void)
{
    s_restored = true;
    FILE *f = fopen(TODAY_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "sd: no today.bin; the day's totals start here");
        return;
    }
    size_t n = fread(s_save, 1, sizeof s_save, f);
    fclose(f);
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

static void sd_service(int64_t now)
{
    if (!sd_up(now)) return;
    spk_clock_t c = clock_get();
    if (!c.have_date) return;                   /* no clock, nothing on the card */
    if (!s_restored) today_restore();

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
       it, before today's is written. */
    if (have_yday && yday.day >= 0 && yday.day != s_logged_yday) {
        s_logged_yday = yday.day;
        daily_put(&yday, calibrated);
    }
    if (new_min) {
        s_logged_min_day = min.day;
        s_logged_min_tod = min.tod_s;
        if (have_today && today.day >= 0) daily_put(&today, calibrated);
        if (saved) today_save(saved);
    }
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
 *   60 ms of silence -> EXIO8 low -> 150 ms more, and the reference back at
 *   its floor -> gate off.
 * The DMA holds up to 96 ms of queued audio, so a write returning means its
 * samples are queued, not played. Before the amp goes off, a queue's worth of
 * extra silence is written, so by the time the last write returns the chime
 * and its 60 ms of silence have all gone out.
 */
static void chime_play(void)
{
    float floor = s_ref_floor_dbfs;
    s_ref_peak_dbfs = NAN;
    gate(true);

    bool ok = out_frames(-1, MS_FRAMES(50));
    if (ok && !quiet_now()) {
        amp_set(true);
        ok = out_frames(-1, MS_FRAMES(AMP_SETTLE_MS))
          && out_frames(0, CHIME_FRAMES)
          && out_frames(-1, MS_FRAMES(CHIME_TAIL_MS) + DMA_BUFFERS * READ_FRAMES);
    }
    amp_set(false);                     /* whatever happened above */

    /* The gate holds 150 ms past the amp, and then until the reference slot
       says the speaker's drive has died away -- up to a second more. */
    vTaskDelay(pdMS_TO_TICKS(GATE_HOLD_MS));
    int waited = GATE_HOLD_MS;
    while (waited < GATE_HOLD_MS + 1000 && !isnan(floor) && s_ref_dbfs > floor + 6.0f) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    float peak = s_ref_peak_dbfs, after = s_ref_dbfs;
    gate(false);
    ESP_LOGI(TAG, "chime: %s; reference floor %.1f, peak %.1f, %.1f at release, %d ms after amp off",
             ok ? "played" : "write failed", (double)floor, (double)peak, (double)after, waited);
}

static int64_t s_chime_us;              /* the last one; 0 before any */

static void chime_service(int64_t now)
{
    if (!s_set.chime || !s_amp_ok || !s_out || !s_in) return;
    if (s_chime_us != 0 && now - s_chime_us < CHIME_EVERY_US) return;
    if (quiet_now()) return;
    soundlevel_now_t n;
    sl_lock();
    soundlevel_now(&s_sl, &n);
    sl_unlock();
    if (!n.have || n.red_run_s < CHIME_RED_S) return;
    s_chime_us = now;
    ESP_LOGI(TAG, "chime: red for %.0f s (LAeq3 %.1f dBA)", (double)n.red_run_s, (double)n.laeq3s);
    chime_play();
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
 * RTC_INT on EXIO4 changing would otherwise leave it asserted.
 */
static void keys_poll(int64_t now)
{
    if (s_io) {
        uint32_t lv = 0;
        if (esp_io_expander_get_level(s_io, 0xFFFF, &lv) == ESP_OK) {
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
    char a[12], b[12], c[12];
    ESP_LOGI(TAG, "noise: LAF %s LAeq3 %s dBA (%s) LAeq1 %s | red %.0fs%s | %s%s%s ring %s chime %s sd %s",
             n.have ? lvl(n.laf, a) : "--.-", n.have ? lvl(n.laeq3s, b) : "--.-",
             n.calibrated ? "cal" : "est", n.have ? lvl(n.laeq1s, c) : "--.-",
             (double)n.red_run_s, s_gated ? " GATED" : "",
             when, have && day < 0 ? " (no date)" : "",
             have && in_hours(tod, s_set.night_from, s_set.night_to) ? " night" : "",
             s_set.ring ? "on" : "off", s_set.chime ? "on" : "off", s_sd ? "ok" : "none");
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
    ESP_LOGI(TAG, "status: card %s; reference floor %s dBFS; I2S read errors %u", s_sd ? "logging" : "none",
             lvl(s_ref_floor_dbfs, a), (unsigned)s_read_errors);
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
    ESP_LOGI(TAG, "ble: \"%s\" means nothing here", text);
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
        clock_set_date(c->day);
        s_rtc_pending = true;
        format_date(c->day, date);
        ESP_LOGI(TAG, "clock: the Mac says %s", date);
        break;
    }
    case CMD_TEXT:
        command(c->text);
        break;
    }
}

/* ---- the control task ----------------------------------------------------- */

static void control_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    int64_t next_line = esp_timer_get_time() + 1000000;
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
        expander_start();
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
    s_cmds = xQueueCreate(8, sizeof(cmd_t));

    if (s_in) xTaskCreatePinnedToCore(audio_task, "audio", 8192, NULL, 10, NULL, 1);
    else ESP_LOGE(TAG, "audio: no ES7210, so nothing is measured; the ring shows only its heartbeat");
    xTaskCreate(led_task, "ring", 4096, NULL, 6, NULL);
    xTaskCreate(control_task, "control", 8192, NULL, 4, NULL);

    if (ble_uart_start(on_message, on_time) != ESP_OK)
        ESP_LOGE(TAG, "ble: start failed; no clock pushes, no commands");
    ESP_LOGI(TAG, "speaker: up");
}
