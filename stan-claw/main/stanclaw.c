/*
 * stan-claw: tap, speak, and it answers aloud and on screen (Deepgram,
 * Claude with a remote MCP server, ElevenLabs); and an MCP server on the
 * local network for its speaker, mics and screen. On a Waveshare
 * ESP32-S3-Touch-LCD-4B. docs/superpowers/specs/2026-10-10-stan-claw-design.md
 *
 * One lock (s_busy) owns the speaker, the mics and the screen: a tap's
 * conversation holds it from the tap to the end of the answer, and each MCP
 * tool call holds it for its own length, waiting up to 30 s for it.
 *
 * Untouched, the screen rests (rest.h): a breathing orb at 45 s, dark 5 min
 * later, and dark at 45 s from 19:00 to 08:00. A tap, BOOT, a knock on the
 * desk or an agent's call wakes it; the first tap only wakes. Outside office
 * hours (Mon-Fri 08:00-16:00) it deep-sleeps instead, BOOT or 08:00 waking it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "nvs_flash.h"

#include "agent.h"
#include "https.h"
#include "audio.h"
#include "config.h"
#include "console.h"
#include "lcd.h"
#include "knock.h"
#include "mcpd.h"
#include "motion.h"
#include "net.h"
#include "rest.h"
#include "rtc.h"
#include "stt.h"
#include "touch.h"
#include "tts.h"
#include "ui.h"
#include "spin.h"
#include "voices.h"

static const char *TAG = "stanclaw";

#define PIN_BOOT     0
#define REC_MAX_S    15
#define ANSWER_US    (30LL * 1000000)
#define BUSY_WAIT_MS 30000

static SemaphoreHandle_t s_busy, s_ui_lock, s_go;
static ui_view_t s_view = { .mode = UI_HOME, .now = -1 };
static char s_heard[600], s_text[2100], s_title[64], s_note[96];
static uint16_t *s_image;              /* PSRAM, 480x480 RGB565 */
static volatile bool s_dirty = true, s_mic;
static int64_t s_answer_at;
static int16_t *s_pcm;                 /* PSRAM, REC_MAX_S (or 30 s for listen) of audio */

static void set_view(ui_mode_t mode, const char *heard, const char *text, const char *title, const char *note);

/* Rest (rest.h): untouched for 45 s the screen breathes, then sleeps. Anything
   holding s_busy counts as use; a touch, BOOT, a knock or an agent's call
   wakes it. The main loop alone sets the backlight and the rest level, but
   on_mic forces the light up at once so the LISTENING banner is always seen. */
#define AMBER_MS 450
static volatile int64_t s_active_at;   /* us: the last touch, press, knock or use */
static volatile bool s_in_use;         /* a conversation or a tool holds s_busy */
static volatile int64_t s_amber_until; /* us: an agent woke it: show the orb amber till then */
static volatile int s_light = -1;      /* the backlight as last set, percent */

static void activity(void) { s_active_at = esp_timer_get_time(); }

/* The listening page's waveform: each bar the loudest of three 20 ms frames,
   on a log scale from the room's hush to a raised voice. */
static uint8_t s_levels[UI_WAVE_BARS];
static int s_nlevels;
static int64_t s_listen_t0;

static void on_level(int rms)
{
    static int peak, frames;
    if (rms > peak) peak = rms;
    if (++frames < 3) return;
    float x = peak <= 40 ? 0.0f : (logf((float)peak) - logf(40.0f)) / (logf(6000.0f) - logf(40.0f));
    uint8_t lv = (uint8_t)(x >= 1.0f ? 255 : x * 255.0f);
    frames = peak = 0;
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    if (s_nlevels == UI_WAVE_BARS) memmove(s_levels, s_levels + 1, UI_WAVE_BARS - 1);
    else s_nlevels++;
    s_levels[s_nlevels - 1] = lv;
    s_dirty = true;
    xSemaphoreGive(s_ui_lock);
}

/* Off hours (rest.h): deep sleep until BOOT, or until the next start of office
   hours -- in steps of at most an hour, so the clock chip can correct the
   ESP32's drifting timer on each brief wake. */
static void deep_sleep_now(bool panel_up)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    long secs = rest_until_office(lt.tm_wday, lt.tm_hour * 60 + lt.tm_min, lt.tm_sec);
    if (secs > 3600) secs = 3600;
    if (secs < 20) secs = 20;
    ESP_LOGI(TAG, "deep sleep for %ld s (BOOT wakes it)", secs);
    if (panel_up) lcd_off_for_sleep();
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    esp_deep_sleep_start();
}

/* ---- settings ---- */

static int s_volume = 70, s_speed = 1, s_model = 0;
static char s_voice_name[32];
static int s_cur = -1;                 /* VOICES index in use, -1 none */
static spin_t s_spin;                  /* the voice wheel */
static int s_previewed = -1;           /* the card whose sample last played */
static int64_t s_settled_at;
static volatile bool s_job;            /* a sample or tone is running */
#define JOB_TONE (-1)

static const char *const SPEEDS[3] = { "0.85", "1.0", "1.15" };
static const char *const MODELS[2] = { "claude-sonnet-5", "claude-haiku-4-5-20251001" };

static void settings_load(void)
{
    char v[CFG_VAL], fx[16];
    cfg_get(CFG_VOLUME, v, sizeof v);
    s_volume = v[0] ? atoi(v) : 70;
    if (s_volume < 0 || s_volume > 100) s_volume = 70;
    audio_set_volume(s_volume);
    cfg_get(CFG_SPEED, v, sizeof v);
    s_speed = strcmp(v, SPEEDS[0]) == 0 ? 0 : strcmp(v, SPEEDS[2]) == 0 ? 2 : 1;
    cfg_get(CFG_MODEL, v, sizeof v);
    s_model = strstr(v, "haiku") ? 1 : 0;
    cfg_get(CFG_VOICE, v, sizeof v);
    cfg_get(CFG_VOICE_FX, fx, sizeof fx);
    s_cur = voices_find(v, fx);
    snprintf(s_voice_name, sizeof s_voice_name, "%s", s_cur >= 0 ? VOICES[s_cur].name : v[0] ? "Another voice" : "");
}

void settings_reload(void) { settings_load(); s_dirty = true; }

static void tone(void)
{
    static int16_t t[AUDIO_FS / 6];
    for (int i = 0; i < AUDIO_FS / 6; i++) {
        float env = i < 400 ? i / 400.0f : (AUDIO_FS / 6 - i) / (float)(AUDIO_FS / 6);
        t[i] = (int16_t)(7000 * env * sinf(2 * 3.14159f * 660 * i / AUDIO_FS));
    }
    audio_play_begin();
    audio_play_chunk(t, AUDIO_FS / 6);
    audio_play_end();
}

static void job_task(void *arg)
{
    int what = (int)(intptr_t)arg;
    if (xSemaphoreTake(s_busy, 0) == pdTRUE) {
        s_in_use = true;
        activity();
        if (what == JOB_TONE) {
            tone();
        } else if (what >= 0 && what < VOICES_N) {
            char text[48];
            snprintf(text, sizeof text, "Hi, I'm %s.", VOICES[what].name);
            tts_speak_as(VOICES[what].id, VOICES[what].fx, text);
        }
        s_in_use = false;
        activity();
        xSemaphoreGive(s_busy);
    }
    s_job = false;
    vTaskDelete(NULL);
}

static void job(int what)
{
    if (s_job) return;
    s_job = true;
    if (xTaskCreatePinnedToCore(job_task, "job", 16384, (void *)(intptr_t)what, 4, NULL, 1) != pdPASS) s_job = false;
}

static void open_voices(void)
{
    spin_init(&s_spin, VOICES_N, s_cur >= 0 ? s_cur : 0);
    s_previewed = spin_index(&s_spin);     /* no sample for the one already in use */
    set_view(UI_VOICES, NULL, NULL, NULL, NULL);
}

static void use_voice(int i)
{
    s_cur = i;
    snprintf(s_voice_name, sizeof s_voice_name, "%s", VOICES[i].name);
    cfg_set(CFG_VOICE, VOICES[i].id);
    cfg_set(CFG_VOICE_FX, fx_name(VOICES[i].fx));
    cfg_set(CFG_VOICE_NAME, VOICES[i].name);
}

/* The page fields of the view, from the settings state (main loop only). */
static void sync_view(void)
{
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_view.voice_name = s_voice_name;
    s_view.volume = s_volume;
    s_view.speed = s_speed;
    s_view.model = s_model;
    s_view.spin = s_spin.pos;
    s_view.cur = s_cur;
    xSemaphoreGive(s_ui_lock);
}

static bool office_now(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return rest_office(lt.tm_wday, lt.tm_hour * 60 + lt.tm_min);
}

static void light(int pct)
{
    if (pct != s_light) { lcd_backlight(pct); s_light = pct; }
}

static void set_view(ui_mode_t mode, const char *heard, const char *text, const char *title, const char *note)
{
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_view.mode = mode;
    snprintf(s_heard, sizeof s_heard, "%s", heard ? heard : "");
    snprintf(s_text, sizeof s_text, "%s", text ? text : "");
    snprintf(s_title, sizeof s_title, "%s", title ? title : "");
    snprintf(s_note, sizeof s_note, "%s", note ? note : "");
    s_view.heard = s_heard; s_view.text = s_text; s_view.title = s_title; s_view.note = s_note;
    s_dirty = true;
    xSemaphoreGive(s_ui_lock);
}

static void on_mic(bool open)
{
    s_mic = open;
    s_dirty = true;
    activity();
    if (open) {
        light(100);
        xSemaphoreTake(s_ui_lock, portMAX_DELAY);
        s_nlevels = 0;                      /* a fresh waveform */
        s_listen_t0 = esp_timer_get_time();
        xSemaphoreGive(s_ui_lock);
    }
    /* Draw now, so the banner is up before the first sample is taken. */
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_view.mic_open = open;
    ui_draw(lcd_canvas(), &s_view);
    lcd_show();
    xSemaphoreGive(s_ui_lock);
}

static const char *service_error(const char *who, int code, char *buf, size_t n)
{
    if (code == 0) snprintf(buf, n, "%s key missing", who);
    else if (code == TTS_NO_VOICE) snprintf(buf, n, "%s voice missing", who);
    else if (code == 401 || code == 403) snprintf(buf, n, "%s key rejected", who);
    else if (code == 402) snprintf(buf, n, "%s: out of credit", who);
    else if (code == 429) snprintf(buf, n, "%s: too many requests", who);
    else if (code < 0) snprintf(buf, n, "%s did not answer", who);
    else snprintf(buf, n, "%s error %d", who, code);
    return buf;
}

/* A tap or BOOT: listen, transcribe, ask, speak. Holds s_busy throughout. */
/* A conversation: listen, transcribe, ask, speak -- and then listen again,
   round after round, until a tap ends it (s_stop) or nobody speaks for 30 s.
   Holds s_busy throughout. Claude keeps the last CLAUDE_TURNS exchanges. */
static volatile bool s_stop, s_convo;
#define CONVO_QUIET_S 30

static void conversation(void)
{
    char err[64];
    if (!net_up()) { set_view(UI_HOME, NULL, NULL, NULL, "no WiFi"); return; }
    static char heard[600];
    static claude_reply_t reply;
    int quiet_s = 0;
    for (int round = 0; !s_stop; round++) {
        set_view(UI_LISTENING, NULL, NULL, round ? "Your turn" : NULL, NULL);
        vad_result_t why;
        int n = audio_record(s_pcm, AUDIO_FS * REC_MAX_S, REC_MAX_S * 1000, &why);
        if (s_stop) break;
        if (why == VAD_SILENT) {
            quiet_s += REC_MAX_S;
            if (round == 0) { set_view(UI_HOME, NULL, NULL, NULL, "didn't catch that"); return; }
            if (quiet_s >= CONVO_QUIET_S) break;
            continue;
        }
        quiet_s = 0;

        set_view(UI_THINKING, "...", NULL, NULL, NULL);
        int code = stt_transcribe(s_pcm, n, heard, sizeof heard);
        if (code != 200) { set_view(UI_ERROR, NULL, "Check the key with: tools/stan-claw.py status", service_error("Deepgram", code, err, sizeof err), NULL); return; }
        if (!heard[0]) continue;
        if (s_stop) break;

        set_view(UI_THINKING, heard, NULL, NULL, NULL);
        code = agent_ask(heard, &reply);
        if (code != 200) {
            set_view(UI_ERROR, heard, reply.error[0] ? reply.error : "", service_error("Claude", code, err, sizeof err), NULL);
            return;
        }
        if (s_stop) break;
        char note[96] = "";
        if (reply.mcp_error) snprintf(note, sizeof note, "MCP server unreachable");
        else if (reply.tools[0]) snprintf(note, sizeof note, "asked remote: %.70s", reply.tools);

        set_view(UI_SPEAKING, heard, reply.text, NULL, note);
        code = tts_speak(reply.text);
        if (code != 200 && !s_stop) {
            s_answer_at = esp_timer_get_time();
            set_view(UI_ANSWER, heard, reply.text, NULL, service_error("ElevenLabs", code, err, sizeof err));
            return;
        }
    }
    s_answer_at = esp_timer_get_time();
    set_view(UI_HOME, NULL, NULL, NULL, s_stop ? NULL : "conversation ended");
}

static void convo_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        if (xSemaphoreTake(s_busy, 0) != pdTRUE) continue;      /* an agent is using it: ignore the tap */
        s_in_use = true;
        activity();
        s_stop = false;
        s_convo = true;
        conversation();
        s_convo = false;
        s_in_use = false;
        activity();
        ESP_LOGI(TAG, "convo stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreTake(s_go, 0);        /* drop a BOOT press that queued during the conversation */
        xSemaphoreGive(s_busy);
    }
}

/* ---- the MCP tools: each takes s_busy for its own length ---- */

/* Waits up to 30 s for the lock. `wake`: the tool wants to be seen or heard,
   so a resting screen flashes the orb amber and comes up. */
static bool take_for(char *out, size_t n, bool wake)
{
    if (xSemaphoreTake(s_busy, pdMS_TO_TICKS(BUSY_WAIT_MS)) != pdTRUE) {
        snprintf(out, n, "busy");
        return false;
    }
    bool resting = s_light != 100;
    s_in_use = true;
    activity();
    if (wake && resting) {
        s_amber_until = esp_timer_get_time() + AMBER_MS * 1000LL;
        vTaskDelay(pdMS_TO_TICKS(AMBER_MS + 60));
    }
    return true;
}

static bool take(char *out, size_t n) { return take_for(out, n, true); }

static void give(void)
{
    s_in_use = false;
    activity();
    xSemaphoreGive(s_busy);
}

static bool t_speak(const char *text, char *out, size_t n)
{
    if (!take(out, n)) return false;
    ui_mode_t was = s_view.mode;
    if (was == UI_HOME) set_view(UI_SPEAKING, NULL, text, NULL, "an agent is speaking");
    int code = tts_speak(text);
    if (was == UI_HOME) set_view(UI_HOME, NULL, NULL, NULL, NULL);
    give();
    ESP_LOGI(TAG, "MCP server stack after speak: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (code == 200) { snprintf(out, n, "spoken"); return true; }
    service_error("ElevenLabs", code, out, n);
    return false;
}

static bool hear(int max_seconds, char *out, size_t n)
{
    vad_result_t why;
    int k = audio_record(s_pcm, AUDIO_FS * max_seconds, max_seconds * 1000, &why);
    if (why == VAD_SILENT) { out[0] = 0; return true; }
    int code = stt_transcribe(s_pcm, k, out, n);
    if (code == 200) return true;
    service_error("Deepgram", code, out, n);
    return false;
}

static bool t_listen(int max_seconds, char *out, size_t n)
{
    if (!take(out, n)) return false;
    bool ok = hear(max_seconds, out, n);
    give();
    return ok;
}

static bool t_ask(const char *question, char *out, size_t n)
{
    if (!take(out, n)) return false;
    set_view(UI_AGENT_TEXT, NULL, question, "An agent asks", NULL);
    int code = tts_speak(question);
    bool ok;
    if (code != 200) { service_error("ElevenLabs", code, out, n); ok = false; }
    else ok = hear(20, out, n);
    give();
    return ok;
}

static bool t_show_text(const char *title, const char *text, char *out, size_t n)
{
    if (!take(out, n)) return false;
    set_view(UI_AGENT_TEXT, NULL, text, title, NULL);
    give();
    snprintf(out, n, "shown");
    return true;
}

static bool t_show_image(const uint8_t *jpeg, size_t len, char *out, size_t n)
{
    if (!take(out, n)) return false;
    /* s_busy is held (mics closed, on_mic can't contend), so it's safe to
       also hold s_ui_lock across the decode: the main loop's redraw can't
       run against a half-decoded s_image, and a bad JPEG that partly
       overwrites s_image before failing can't be shown either. */
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    esp_jpeg_image_cfg_t jc = {
        .indata = (uint8_t *)jpeg, .indata_size = len,
        .outbuf = (uint8_t *)s_image, .outbuf_size = 480 * 480 * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t jo;
    bool ok = esp_jpeg_decode(&jc, &jo) == ESP_OK && jo.width <= 480 && jo.height <= 480;
    if (ok) {
        s_view.mode = UI_AGENT_IMAGE;
        s_view.image = s_image;
        s_view.img_w = jo.width;
        s_view.img_h = jo.height;
        s_dirty = true;
    } else if (s_view.mode == UI_AGENT_IMAGE) {
        s_view.mode = UI_HOME;
        s_dirty = true;
    }
    xSemaphoreGive(s_ui_lock);
    if (ok) snprintf(out, n, "shown");
    else snprintf(out, n, "could not decode that JPEG (baseline, at most 480x480)");
    give();
    return ok;
}

static bool t_clear(char *out, size_t n)
{
    if (!take_for(out, n, false)) return false;
    set_view(UI_HOME, NULL, NULL, NULL, NULL);
    give();
    snprintf(out, n, "cleared");
    return true;
}

static bool t_volume(int level, char *out, size_t n)
{
    audio_set_volume(level);
    s_volume = audio_volume();
    char v[8];
    snprintf(v, sizeof v, "%d", s_volume);
    cfg_set(CFG_VOLUME, v);
    s_dirty = true;
    snprintf(out, n, "%d", audio_volume());
    return true;
}

static bool t_status(char *out, size_t n)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif) esp_netif_get_ip_info(nif, &ip);
    bool idle = xSemaphoreTake(s_busy, 0) == pdTRUE;
    if (idle) xSemaphoreGive(s_busy);
    snprintf(out, n, "wifi %s, ip " IPSTR ", volume %d, %s", net_up() ? net_ssid() : "none", IP2STR(&ip.ip),
             audio_volume(), idle ? "idle" : "busy");
    return true;
}

static const mcp_tools_t TOOLS = { t_speak, t_listen, t_ask, t_show_text, t_show_image, t_clear, t_volume, t_status };

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    cfg_load();

    /* The time from the clock chip, before anything else: a timer wake in
       off hours goes straight back to sleep, the panel never lit. */
    lcd_bus_init();
    clockchip_init();
    time_t rt;
    bool clock_from_rtc = clockchip_get(&rt);
    if (clock_from_rtc) {
        struct timeval tv = { .tv_sec = rt };
        settimeofday(&tv, NULL);
    }
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && clock_from_rtc && !office_now())
        deep_sleep_now(false);

    s_busy = xSemaphoreCreateMutex();
    s_ui_lock = xSemaphoreCreateMutex();
    s_go = xSemaphoreCreateBinary();
    s_pcm = heap_caps_malloc(AUDIO_FS * 30 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_image = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);

    ESP_ERROR_CHECK(lcd_init());
    bool touch = touch_init() == ESP_OK;
    motion_init();
    static knock_t knock;
    knock_init(&knock);
    activity();
    if (audio_init() != ESP_OK) set_view(UI_ERROR, NULL, "The speaker or microphones did not start.", "Audio failed", NULL);
    audio_on_mic(on_mic);
    audio_on_level(on_level);
    settings_load();
    gpio_config_t boot = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);

    net_start();
    console_start();
    mcpd_on_request(activity);
    mcpd_start(&TOOLS);
    xTaskCreatePinnedToCore(convo_task, "convo", 16384, NULL, 5, NULL, 1);      /* TLS x3 */

    bool down = false, boot_was = true;
    int last_min = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int x, y;
        bool now_down = touch && touch_read(&x, &y);
        bool boot_now = gpio_get_level(PIN_BOOT) != 0;
        bool awake = s_light == 100;
        sync_view();
        static bool ignore, moved;
        static int px, py, lx, ly, pval, prevx;
        static int64_t last_t;
        static float vx;
        static ui_hit_t phit;
        if (now_down) { lx = x; ly = y; }
        if (now_down && !down) {                                    /* a press */
            activity();
            ignore = !awake;                                        /* on a resting screen it only wakes */
            moved = false;
            px = prevx = x; py = y;
            vx = 0;
            last_t = esp_timer_get_time();
            if (!ignore && s_view.mode == UI_VOICES) spin_grab(&s_spin);
            phit = ignore ? UI_HIT_NONE : ui_hit(&s_view, x, y, &pval);
            ui_mode_t m = s_view.mode;
            if (ignore) {
            } else if (m == UI_HOME) {
                if (phit == UI_HIT_TALK) xSemaphoreGive(s_go);
                else if (phit == UI_HIT_GEAR) set_view(UI_SETTINGS, NULL, NULL, NULL, NULL);
            } else if (s_convo) {
                s_stop = true;                                      /* a tap ends the conversation */
                audio_abort();
                tts_abort();
            } else if (m != UI_SETTINGS && m != UI_VOICES && m != UI_LISTENING && m != UI_THINKING && m != UI_SPEAKING) {
                set_view(UI_HOME, NULL, NULL, NULL, NULL);          /* a tap clears answers and agent screens */
            }
        } else if (now_down && down && !ignore) {                   /* a drag */
            activity();
            if (s_view.mode == UI_SETTINGS && phit == UI_HIT_VOLUME) {
                int vol = ui_slider_value(x);
                if (vol != s_volume) { s_volume = vol; audio_set_volume(vol); s_dirty = true; }
            } else if (s_view.mode == UI_VOICES) {
                if (abs(x - px) > 10) moved = true;
                spin_drag(&s_spin, (float)(x - px));
                int64_t t = esp_timer_get_time();
                float dt = (t - last_t) / 1e6f;
                if (dt > 0.005f) {                                  /* the finger's speed, smoothed, for a flick */
                    vx = 0.5f * vx + 0.5f * (x - prevx) / dt;
                    prevx = x;
                    last_t = t;
                }
                s_dirty = true;
            }
        } else if (!now_down && down && !ignore) {                  /* a release */
            ui_mode_t m = s_view.mode;
            if (m == UI_SETTINGS) {
                if (phit == UI_HIT_VOLUME) {
                    char v[8];
                    snprintf(v, sizeof v, "%d", s_volume);
                    cfg_set(CFG_VOLUME, v);
                    job(JOB_TONE);
                } else if (!moved && phit == UI_HIT_BACK) {
                    set_view(UI_HOME, NULL, NULL, NULL, NULL);
                } else if (!moved && phit == UI_HIT_VOICE) {
                    open_voices();
                } else if (!moved && phit == UI_HIT_SPEED) {
                    s_speed = pval;
                    cfg_set(CFG_SPEED, SPEEDS[pval]);
                    s_dirty = true;
                } else if (!moved && phit == UI_HIT_MODEL) {
                    s_model = pval;
                    cfg_set(CFG_MODEL, MODELS[pval]);
                    s_dirty = true;
                }
            } else if (m == UI_VOICES) {
                (void)lx; (void)ly; (void)py;
                if (moved) {
                    /* A finger that stopped before lifting is not a flick. */
                    if (esp_timer_get_time() - last_t > 80000) vx = 0;
                    spin_release(&s_spin, vx);
                } else {
                    spin_release(&s_spin, 0);
                    if (phit == UI_HIT_BACK) {
                        set_view(UI_SETTINGS, NULL, NULL, NULL, NULL);
                    } else if (phit == UI_HIT_CARD && pval == spin_index(&s_spin)) {
                        job(pval);                                  /* the middle card again: hear it again */
                    } else if (phit == UI_HIT_CARD) {
                        spin_go(&s_spin, pval);                     /* a side card: turn to it */
                    } else if (phit == UI_HIT_USE && spin_index(&s_spin) != s_cur) {
                        use_voice(spin_index(&s_spin));
                        set_view(UI_SETTINGS, NULL, NULL, NULL, NULL);
                    }
                }
                s_dirty = true;
            }
        }
        if (!boot_now && boot_was) {
            if (awake) xSemaphoreGive(s_go);
            activity();
        }
        if (!awake) {
            float ax, ay, az;
            if (motion_read(&ax, &ay, &az) && knock_feed(&knock, ax, ay, az, (long)(esp_timer_get_time() / 1000)))
                activity();
        }
        down = now_down;
        boot_was = boot_now;

        static bool was_up;
        if (net_up() != was_up) { was_up = net_up(); s_dirty = true; }   /* the network name at the top */
        static unsigned tick;
        if (s_view.mode == UI_LISTENING && (++tick & 1)) s_dirty = true;   /* the waveform and timer move, ~16 fps */
        if (s_view.mode == UI_VOICES) {
            /* Turn the wheel; once it has rested on a new card for a moment,
               that voice says hello. */
            if (spin_step(&s_spin, 0.03f)) { s_dirty = true; s_settled_at = esp_timer_get_time(); }
            int at = spin_index(&s_spin);
            if (!s_spin.moving && at != s_previewed && esp_timer_get_time() - s_settled_at > 350000) {
                s_previewed = at;
                job(at);
            }
        }
        if ((s_view.mode == UI_SETTINGS || s_view.mode == UI_VOICES)
            && esp_timer_get_time() - s_active_at > REST_BREATHE_S * 1000000LL)
            set_view(UI_HOME, NULL, NULL, NULL, NULL);
        if (s_view.mode == UI_ANSWER && esp_timer_get_time() - s_answer_at > ANSWER_US)
            set_view(UI_HOME, NULL, NULL, NULL, NULL);
        if (s_view.mode == UI_HOME) {
            const char *want = !net_up() ? "no WiFi" : !cfg_has(CFG_CLAUDE_KEY) ? "Claude key missing"
                             : !cfg_has(CFG_DEEPGRAM_KEY) ? "Deepgram key missing" : NULL;
            if (want && strcmp(s_note, want) != 0) set_view(UI_HOME, NULL, NULL, NULL, want);
            if (!want && s_note[0] && (strcmp(s_note, "no WiFi") == 0 || strstr(s_note, "key missing")))
                set_view(UI_HOME, NULL, NULL, NULL, NULL);
        }
        if (net_time_ok()) {
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            int m = lt.tm_hour * 60 + lt.tm_min;
            if (m != last_min) { last_min = m; s_view.now = m; s_dirty = true; }
        }
        /* A fresh time from the network goes into the clock chip. */
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) clockchip_set(time(NULL));

        /* How awake to be, and the light for it. */
        int64_t now_us = esp_timer_get_time();
        int idle_s = s_in_use || s_mic ? 0 : (int)((now_us - s_active_at) / 1000000);
        if (net_time_ok() && !office_now() && idle_s >= REST_BREATHE_S) deep_sleep_now(true);
        rest_level_t want = rest_level(idle_s, net_time_ok() ? s_view.now : -1);
        bool amber = now_us < s_amber_until;
        static rest_level_t shown = REST_AWAKE;
        static bool shown_amber;
        static int64_t rest_t0;
        bool resting_screen = amber || want != REST_AWAKE;
        if ((shown != REST_AWAKE) != resting_screen || amber != shown_amber) s_dirty = true;
        if (want == REST_BREATHING && shown != REST_BREATHING) rest_t0 = now_us;
        shown = amber ? REST_BREATHING : want;
        shown_amber = amber;
        light(amber ? 60 : want == REST_AWAKE ? 100 : want == REST_ASLEEP ? 0
              : rest_breath_light((long)((now_us - rest_t0) / 1000)));

        if (s_dirty) {
            xSemaphoreTake(s_ui_lock, portMAX_DELAY);
            s_dirty = false;
            s_view.mic_open = s_mic;
            s_view.ssid = net_up() ? net_ssid() : NULL;
            s_view.levels = s_levels;
            s_view.nlevels = s_nlevels;
            s_view.listen_ms = (int)((now_us - s_listen_t0) / 1000);
            if (resting_screen) {
                ui_view_t rv = { .mode = UI_RESTING, .now = s_view.now, .amber = amber, .mic_open = s_mic };
                ui_draw(lcd_canvas(), &rv);
            } else {
                ui_draw(lcd_canvas(), &s_view);
            }
            lcd_show();
            xSemaphoreGive(s_ui_lock);
        }
    }
}
