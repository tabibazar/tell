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
 * desk or an agent's call wakes it; the first tap only wakes. It is a home
 * device, up day and night (ALWAYS_ON); with ALWAYS_ON 0 it deep-sleeps
 * outside office hours (Mon-Fri 08:00-16:00), as it did at the office.
 *
 * Care (docs/superpowers/specs/2026-10-10-stan-claw-care-design.md): in a
 * conversation big buttons repeat the answer, repeat it slowly, or end it, and
 * a question that needs a yes gets a Yes/No card; captions follow the speech;
 * two knocks start a conversation. A care task fetches the homeowner's
 * reminders and family's messages from stan-mcp, speaks them 9 am-7 pm with a
 * chime and a card, and the board wakes from deep sleep for a reminder.
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
#include "care_net.h"
#include "phrases.h"
#include "weather.h"
#include "https.h"
#include "audio.h"
#include "config.h"
#include "console.h"
#include "lcd.h"
#include "knock.h"
#include "mcpd.h"
#include "memory.h"
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
#define ALWAYS_ON    1           /* a home device: up day and night, the screen resting on its own */
#define REC_MAX_S    25          /* long enough for a slow, careful question */
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
static volatile bool s_stop, s_convo;

/* Conversation buttons and the Yes/No card: the touch handler sets s_action
   and cuts the recording short; conversation() acts on it. */
enum { ACT_NONE, ACT_AGAIN, ACT_SLOWER, ACT_YES, ACT_NO };
static volatile int s_action;
static char s_confirm[160];            /* the Yes/No card's summary while it listens, "" for none */
/* Captions: while it speaks, how far through the text it is (tts_played). */
static volatile bool s_captions;
static volatile float s_cap_rate;      /* characters a second at the speed in use */
static volatile bool s_care_wake;      /* woke from deep sleep for a reminder */
/* When the person last did something themselves (a touch, BOOT, a knock, a
   conversation), wall clock; kept through deep sleep for the morning check-in. */
static RTC_DATA_ATTR time_t s_person_at;
static uint16_t *s_card_img, *s_rest_img;   /* PSRAM 480x480: a photo card, the resting photo */
static weather_t s_weather;                  /* for the resting screen, from Open-Meteo every 15 minutes */
#define WEATHER_HOME "43.65,-79.38"          /* Toronto: until the internet connection says where home is */
/* Where the home is, from its internet connection (ipinfo.io, else ipapi.co),
   looked up at start and once a day; `weather at LAT,LON` overrides it. */
static volatile bool s_geo_ok;
static double s_geo_lat, s_geo_lon;
static char s_geo_city[40];

void weather_where(char *out, size_t n)
{
    char at[CFG_VAL];
    cfg_get(CFG_WEATHER_AT, at, sizeof at);
    if (at[0]) snprintf(out, n, "%s (set with weather at; weather at auto goes back to the internet's guess)", at);
    else if (s_geo_ok) snprintf(out, n, "%s, %.2f,%.2f (from the internet connection)", s_geo_city[0] ? s_geo_city : "?", s_geo_lat, s_geo_lon);
    else snprintf(out, n, "Toronto (the default, until the internet connection is asked)");
}

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
    long remind = care_reminders() ? care_next_wake(care_reminders(), (long)t) : 0;   /* a reminder to say */
    if (remind > 0 && remind - t < secs) secs = remind - t;
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
static int s_textsize;                 /* see and hear: 0 large, 1 extra large */
static bool s_contrast, s_clarity = true;
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
    cfg_get(CFG_TEXTSIZE, v, sizeof v);
    s_textsize = v[0] == '1';
    cfg_get(CFG_CONTRAST, v, sizeof v);
    s_contrast = v[0] == '1';
    cfg_get(CFG_CLARITY, v, sizeof v);
    s_clarity = v[0] != '0';
}

void settings_reload(void) { settings_load(); s_dirty = true; }

/* A soft two-note chime before it speaks up on its own (made once, in PSRAM). */
static void chime(void)
{
    static int16_t *t;
    const int n = AUDIO_FS * 3 / 5, half = n / 2;
    if (!t && !(t = heap_caps_malloc(n * sizeof *t, MALLOC_CAP_SPIRAM))) return;
    for (int i = 0; i < n; i++) {
        int k = i < half ? i : i - half;
        float env = (k < 300 ? k / 300.0f : 1.0f) * expf(-3.5f * k / half);
        float f = i < half ? 659.0f : 880.0f;
        t[i] = (int16_t)(6000 * env * sinf(2 * 3.14159f * f * k / AUDIO_FS));
    }
    audio_play_begin();
    audio_play_chunk(t, n);
    audio_play_end();
}

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
    s_view.textsize = s_textsize;
    s_view.contrast = s_contrast;
    s_view.clarity = s_clarity;
    s_view.convo = s_convo;
    s_view.confirm = s_confirm;
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
    s_view.spoken = -1;                    /* all bright, until captions move it */
    s_view.image = NULL;                   /* a photo card sets its own after */
    s_view.photo_from = NULL;
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
   round after round, until the person signs off ("that's all" -- Claude's
   [END], or their own words), a tap ends it (s_stop), or nobody answers
   within 12 s twice -- after a kind "anything else?" in between.
   Holds s_busy throughout. Claude keeps the last CLAUDE_TURNS exchanges. */
#define CONVO_WAIT_MS 12000   /* after an answer: this long to start talking before it checks in */

/* Speaks `text` with captions following it, at `speed` (0: the setting). */
static int say(const char *text, float speed)
{
    s_cap_rate = 14.5f * (speed > 0 ? speed : tts_speed());
    s_captions = true;
    int code = speed > 0 ? tts_speak_slow(text, speed) : tts_speak(text);
    s_captions = false;
    xSemaphoreTake(s_ui_lock, portMAX_DELAY);
    s_view.spoken = -1;
    s_dirty = true;
    xSemaphoreGive(s_ui_lock);
    return code;
}

/* "One moment...": if Claude takes longer than FILL_MS, a recorded phrase
   fills the silence. The answer waits for the phrase to finish (one speaker). */
#define FILL_MS 2500
static SemaphoreHandle_t s_fill_go, s_fill_done;
static volatile bool s_fill_cancel;

static void filler_task(void *arg)
{
    static int next;
    for (;;) {
        xSemaphoreTake(s_fill_go, portMAX_DELAY);
        for (int t = 0; t < FILL_MS && !s_fill_cancel; t += 50) vTaskDelay(pdMS_TO_TICKS(50));
        if (!s_fill_cancel && !s_stop) {
            phrase_play((phrase_t)(PH_MOMENT1 + next));
            next = (next + 1) % 3;
        }
        xSemaphoreGive(s_fill_done);
    }
}

/* Says a recorded phrase with its words on screen: speech that needs no network. */
static void plain_words(ui_mode_t mode, phrase_t p, const char *title, const char *note)
{
    set_view(mode, NULL, phrase_text(p), title, note);
    phrase_play(p);
}

/* The "how are you today?" check-in opens a conversation itself: true if anyone answered. */
static const char *volatile s_opening;
static volatile bool s_answered;

static void conversation(void)
{
    char err[64];
    const char *opening = s_opening;
    s_opening = NULL;
    s_answered = false;
    if (!net_up()) {
        if (!opening) plain_words(UI_HOME, PH_OFFLINE, NULL, "no WiFi");
        return;
    }
    static char heard[600], last[1500], last_heard[600], last_note[96];
    static claude_reply_t reply;
    bool checked_in = false;
    last[0] = s_confirm[0] = 0;
    s_action = ACT_NONE;
    if (opening) {
        set_view(UI_SPEAKING, NULL, opening, NULL, NULL);
        say(opening, 0);
    }
    for (int round = 0; !s_stop; round++) {
        set_view(UI_LISTENING, NULL, NULL, round ? "Your turn" : opening ? "How are you today?" : NULL, NULL);
        vad_result_t why;
        int n = audio_record_wait(s_pcm, AUDIO_FS * REC_MAX_S, REC_MAX_S * 1000,
                                  round ? CONVO_WAIT_MS : opening ? 20000 : REC_MAX_S * 1000, &why);
        if (why != VAD_SILENT || s_action != ACT_NONE) s_answered = true;
        if (s_stop) break;
        int act = s_action;
        s_action = ACT_NONE;
        if (act == ACT_AGAIN || act == ACT_SLOWER) {
            /* "Say it again" / "slower please": the last answer, once more
               (before any answer, just listen again). */
            if (last[0]) {
                set_view(UI_SPEAKING, last_heard, last, NULL, last_note);
                say(last, act == ACT_SLOWER ? 0.75f : 0);
            }
            round = round > 0 ? round - 1 : -1;
            continue;
        }
        if (act == ACT_YES || act == ACT_NO) {
            snprintf(heard, sizeof heard, "%s", act == ACT_YES ? "Yes." : "No.");
        } else {
            if (why == VAD_SILENT) {
                if (round == 0) {
                    s_confirm[0] = 0;
                    if (opening) { set_view(UI_HOME, NULL, NULL, NULL, NULL); return; }   /* the check-in tries later */
                    plain_words(UI_HOME, PH_MISSED, NULL, "didn't catch that");
                    return;
                }
                /* No reply: check in once, kindly, before letting them go. */
                if (!checked_in) {
                    checked_in = true;
                    set_view(UI_SPEAKING, NULL, "Is there anything else I can help you with?", NULL, NULL);
                    say("Is there anything else I can help you with?", 0);
                    continue;
                }
                s_confirm[0] = 0;
                set_view(UI_SPEAKING, NULL, "All right. Just tap the screen whenever you need me. Goodbye.", NULL, NULL);
                say("All right. Just tap the screen whenever you need me. Goodbye.", 0);
                break;
            }
            set_view(UI_THINKING, "...", NULL, NULL, NULL);
            int code = stt_transcribe(s_pcm, n, heard, sizeof heard);
            if (code != 200) {
                s_confirm[0] = 0;
                set_view(UI_ERROR, NULL, phrase_text(PH_TROUBLE), service_error("Deepgram", code, err, sizeof err), NULL);
                phrase_play(PH_TROUBLE);
                return;
            }
            if (!heard[0]) continue;
            if (s_stop) break;
        }
        checked_in = false;
        s_confirm[0] = 0;

        set_view(UI_THINKING, heard, NULL, NULL, NULL);
        s_fill_cancel = false;
        xSemaphoreGive(s_fill_go);
        int code = agent_ask(heard, &reply);
        s_fill_cancel = true;
        xSemaphoreTake(s_fill_done, portMAX_DELAY);     /* "one moment" finishes before the answer starts */
        if (code != 200) {
            set_view(UI_ERROR, heard, phrase_text(PH_TROUBLE), service_error("Claude", code, err, sizeof err), NULL);
            phrase_play(PH_TROUBLE);
            return;
        }
        if (s_stop) break;
        bool end = reply.end || claude_is_signoff(heard);
        char note[96] = "";
        if (reply.mcp_error) snprintf(note, sizeof note, "I couldn't reach the HOA service just now");
        else if (reply.tools[0]) snprintf(note, sizeof note, "I checked your HOA records");
        snprintf(last, sizeof last, "%s", reply.text);
        snprintf(last_heard, sizeof last_heard, "%s", heard);
        snprintf(last_note, sizeof last_note, "%s", note);

        set_view(UI_SPEAKING, heard, reply.text, NULL, note);
        code = say(reply.text, 0);
        if (code != 200 && !s_stop) {
            s_answer_at = esp_timer_get_time();
            set_view(UI_ANSWER, heard, reply.text, NULL, service_error("ElevenLabs", code, err, sizeof err));
            phrase_play(PH_TROUBLE);              /* recorded: it needs no ElevenLabs */
            return;
        }
        if (end) break;                                     /* "that's all": the goodbye was the last word */
        if (reply.confirm[0]) snprintf(s_confirm, sizeof s_confirm, "%s", reply.confirm);   /* Yes / No, next round */
    }
    s_confirm[0] = 0;
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
        if (s_answered) s_person_at = time(NULL);
        s_in_use = false;
        activity();
        ESP_LOGI(TAG, "convo stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreTake(s_go, 0);        /* drop a BOOT press that queued during the conversation */
        xSemaphoreGive(s_busy);
    }
}

/* ---- care: reminders and family messages ---- */

static volatile bool s_care_kick;
static volatile int s_care_demo = -1;
static int64_t s_card_at;
void care_kick(void) { s_care_kick = true; }
void care_demo(int kind) { s_care_demo = kind; }

static int minute_now(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour * 60 + lt.tm_min;
}

/* Puts a card up and says it, holding s_busy; only when nobody is using it. */
static bool decode_jpeg(const uint8_t *jpeg, size_t len, uint16_t *out, int *w, int *h)
{
    esp_jpeg_image_cfg_t jc = {
        .indata = (uint8_t *)jpeg, .indata_size = len,
        .outbuf = (uint8_t *)out, .outbuf_size = 480 * 480 * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565, .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t jo;
    if (esp_jpeg_decode(&jc, &jo) != ESP_OK || jo.width > 480 || jo.height > 480) return false;
    *w = jo.width;
    *h = jo.height;
    return true;
}

/* photo: a photo id to show on the card ("" for none). */
static bool present_photo(ui_mode_t mode, const char *title, const char *text, const char *spoken, const char *photo)
{
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) return false;
    s_in_use = true;
    activity();
    vTaskDelay(pdMS_TO_TICKS(250));            /* the main loop brings the light up */
    set_view(mode, NULL, text, title, NULL);
    unsigned char *jpeg = NULL;
    int len = photo && photo[0] && s_card_img ? care_photo_load(photo, &jpeg) : -1;
    int w, h;
    if (len > 0 && decode_jpeg(jpeg, (size_t)len, s_card_img, &w, &h)) {
        xSemaphoreTake(s_ui_lock, portMAX_DELAY);
        s_view.image = s_card_img;
        s_view.img_w = w;
        s_view.img_h = h;
        s_dirty = true;
        xSemaphoreGive(s_ui_lock);
    }
    free(jpeg);
    chime();
    tts_speak(spoken);
    s_card_at = esp_timer_get_time();
    s_in_use = false;
    activity();
    xSemaphoreGive(s_busy);
    return true;
}

static bool present(ui_mode_t mode, const char *title, const char *text, const char *spoken)
{
    return present_photo(mode, title, text, spoken, NULL);
}

/* The morning check-in (if the homeowner agreed): every day from 11:00, if
   nobody has touched it or talked to it since 05:00, it says good morning and
   listens; nothing back, it asks once more 20 minutes later, and then lets
   family know (stan-mcp emails them, once a day). Weekdays only when the
   board sleeps outside office hours (ALWAYS_ON 0). */
static RTC_DATA_ATTR int s_ck_day, s_ck_state;       /* 0 not yet, 1 asked once, 3 settled for the day */
static RTC_DATA_ATTR time_t s_ck_asked;
static bool s_ck_test;                               /* `care test checkin`: now, and 60 s apart */

static bool checkin_ask(bool test)
{
    if (test) { s_ck_test = true; s_ck_state = 0; }
    int m = minute_now();
    s_opening = m < 12 * 60 ? "Good morning! I'm just checking in. How are you today?"
                            : "Hello! I'm just checking in. How are you today?";
    s_answered = false;
    xSemaphoreGive(s_go);
    int64_t t0 = esp_timer_get_time();
    while (!s_convo && esp_timer_get_time() - t0 < 3000000) vTaskDelay(pdMS_TO_TICKS(50));
    while (s_convo) vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_answered && s_ck_state == 0) { s_ck_state = 1; s_ck_asked = time(NULL); }
    else if (s_answered) s_ck_state = 3;
    ESP_LOGI(TAG, "check-in: %s", s_answered ? "answered" : "no answer");
    return s_answered;
}

static void checkin_tick(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int today = (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
    if (s_ck_day != today) { s_ck_day = today; s_ck_state = 0; }
    if (s_ck_state >= 3 || (!s_ck_test && !care_checkin_on())) return;
    struct tm five = lt;
    five.tm_hour = 5; five.tm_min = five.tm_sec = 0;
    if (s_person_at >= mktime(&five) && (s_ck_state > 0 || !s_ck_test)) { s_ck_state = 3; return; }   /* they're about */
    int m = lt.tm_hour * 60 + lt.tm_min;
    bool weekend = lt.tm_wday == 0 || lt.tm_wday == 6;
    if (s_ck_state == 0 && !s_ck_test && ((!ALWAYS_ON && weekend) || m < 11 * 60 || m >= 15 * 60)) return;
    if (s_ck_state == 1 && now - s_ck_asked < (s_ck_test ? 60 : 20 * 60)) return;
    bool second = s_ck_state == 1;
    if (checkin_ask(false)) { s_ck_test = false; return; }
    if (second) {
        char asked[48];
        struct tm a;
        localtime_r(&s_ck_asked, &a);
        snprintf(asked, sizeof asked, "at %d:%02d and %d:%02d", a.tm_hour, a.tm_min, lt.tm_hour, lt.tm_min);
        care_net_checkin_missed(asked);
        s_ck_state = 3;
        s_ck_test = false;
    }
}

static void care_task(void *arg)
{
    const int64_t INBOX_US = 120LL * 1000000, REMIND_US = 3LL * 3600 * 1000000;
    int64_t inbox_at = -INBOX_US, remind_at = -REMIND_US, photos_at = -REMIND_US, phrase_at = -60LL * 1000000;
    int64_t weather_at = -3600LL * 1000000, geo_at = -48LL * 3600 * 1000000;
    char spoken[CARE_TEXT + 48], title[48];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        int64_t now_us = esp_timer_get_time();
        if (net_up() && care_net_ready() && !s_in_use && !s_convo) {     /* never a TLS session beside a conversation's */
            if (s_care_kick || now_us - remind_at >= REMIND_US) {
                /* A failure tries again in 10 minutes. */
                remind_at = care_net_refresh_reminders() ? now_us : now_us - REMIND_US + 600LL * 1000000;
            }
            bool photos_due = s_care_kick || now_us - photos_at >= REMIND_US;
            if (s_care_kick || now_us - inbox_at >= INBOX_US) {
                care_net_check_inbox();
                inbox_at = now_us;
                int mi = care_next_message();               /* a photo announced: fetch it before showing */
                if (mi >= 0 && care_messages()->item[mi].photo[0]) photos_due = true;
            }
            if (photos_due) photos_at = care_net_sync_photos() >= 0 ? now_us : now_us - REMIND_US + 600LL * 1000000;
            s_care_kick = false;
        }
        /* Where home is: at start, then once a day (in 10 minutes after a failure). */
        char at[CFG_VAL];
        cfg_get(CFG_WEATHER_AT, at, sizeof at);
        if (!at[0] && net_up() && !s_in_use && !s_convo
            && now_us - geo_at >= (s_geo_ok ? 24LL * 3600 : 600LL) * 1000000) {
            geo_at = now_us;
            static const char *const geo[2] = { WEATHER_GEO_1, WEATHER_GEO_2 };
            for (int i = 0; i < 2; i++) {
                char *resp = NULL;
                double la, lo;
                char city[40];
                bool ok = https_get(geo[i], NULL, 0, 10000, &resp, 4096) == 200 && resp
                          && weather_geo_parse(resp, &la, &lo, city, sizeof city);
                free(resp);
                if (!ok) continue;
                bool moved = !s_geo_ok || fabs(la - s_geo_lat) > 0.05 || fabs(lo - s_geo_lon) > 0.05;
                s_geo_lat = la; s_geo_lon = lo;
                snprintf(s_geo_city, sizeof s_geo_city, "%s", city);
                s_geo_ok = true;
                if (moved) weather_at = -3600LL * 1000000;        /* the weather for the new place, now */
                ESP_LOGI(TAG, "home is %s (%.2f, %.2f)", city, la, lo);
                break;
            }
        }
        /* The weather for the resting screen: every 15 minutes, again in 2 after a failure. */
        if (net_up() && !s_in_use && !s_convo && now_us - weather_at >= (s_weather.ok ? 900LL : 120LL) * 1000000) {
            weather_at = now_us;
            char url[400];
            double lat = 0, lon = 0;
            if (at[0] && sscanf(at, "%lf,%lf", &lat, &lon) == 2) {
            } else if (s_geo_ok) {
                lat = s_geo_lat; lon = s_geo_lon;
            } else {
                sscanf(WEATHER_HOME, "%lf,%lf", &lat, &lon);
            }
            weather_url(lat, lon, url, sizeof url);
            char *resp = NULL;
            weather_t w;
            if (https_get(url, NULL, 0, 15000, &resp, 8192) == 200 && resp && weather_parse(resp, &w)) {
                xSemaphoreTake(s_ui_lock, portMAX_DELAY);
                s_weather = w;
                s_dirty = true;
                xSemaphoreGive(s_ui_lock);
            }
            free(resp);
        }
        /* The recorded phrases, one at a time, whenever the voice or speed changed. */
        if (net_up() && !s_in_use && !s_convo && !s_job && phrases_stale() && now_us - phrase_at > 20LL * 1000000) {
            if (!phrases_refresh_one()) phrase_at = now_us;            /* failed: wait a little */
        }
        bool idle = !s_in_use && !s_mic && !s_convo && !s_job && s_view.mode != UI_SETTINGS
                    && s_view.mode != UI_VOICES && s_view.mode != UI_EASY;
        /* One card at a time: the next waits for a tap, or 90 s. */
        bool card_up = (s_view.mode == UI_MESSAGE || s_view.mode == UI_REMINDER)
                       && now_us - s_card_at < 90LL * 1000000;
        if (!idle || card_up) continue;
        if (s_care_demo >= 0) {
            int kind = s_care_demo;
            s_care_demo = -1;
            if (kind == 2) { s_ck_test = true; s_ck_state = 0; s_ck_day = 0; continue; }
            if (kind == 1) present(UI_MESSAGE, "Message from Reza", "Hi! I'll call you tonight at seven. Love you.",
                                   "Message from Reza. Hi! I'll call you tonight at seven. Love you.");
            else present(UI_REMINDER, "Reminder", "Tonight is trash night. The bins can go out to the curb now.",
                         "Tonight is trash night. The bins can go out to the curb now.");
            continue;
        }
        if (!net_time_ok()) continue;
        if (net_up()) checkin_tick();
        if (!care_may_speak(minute_now())) continue;
        int mi = care_next_message();
        if (mi >= 0) {
            care_item_t *m = &care_messages()->item[mi];
            if (m->photo[0]) {
                snprintf(title, sizeof title, "A photo from %s", m->from[0] ? m->from : "family");
                snprintf(spoken, sizeof spoken, "%s sent you a photo. %s", m->from[0] ? m->from : "Your family", m->text);
            } else {
                snprintf(title, sizeof title, "Message from %s", m->from[0] ? m->from : "family");
                snprintf(spoken, sizeof spoken, "%s. %s", title, m->text);
            }
            if (present_photo(UI_MESSAGE, title, m->text, spoken, m->photo)) { m->done = true; care_net_save(); }
            continue;
        }
        int ri = care_due(care_reminders(), (long)time(NULL), minute_now());
        if (ri >= 0) {
            care_item_t *r = &care_reminders()->item[ri];
            if (present(UI_REMINDER, "Reminder", r->text, r->text)) { r->done = true; care_net_save(); }
        }
        static int64_t hw_at;
        if (now_us - hw_at > 600LL * 1000000) {
            hw_at = now_us;
            ESP_LOGI(TAG, "care stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
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
    memory_init();

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
    care_net_load();
    if (!ALWAYS_ON && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && clock_from_rtc && !office_now()) {
        /* Woken for a reminder (due, or a moment away): stay up to say it. */
        time_t now = time(NULL);
        long next = care_next_wake(care_reminders(), (long)now);
        if (care_due(care_reminders(), (long)now, minute_now()) >= 0 || (next > 0 && next - now < 90)) s_care_wake = true;
        else deep_sleep_now(false);
    }

    s_busy = xSemaphoreCreateMutex();
    s_ui_lock = xSemaphoreCreateMutex();
    s_go = xSemaphoreCreateBinary();
    s_pcm = heap_caps_malloc(AUDIO_FS * 30 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_image = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
    s_card_img = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
    s_rest_img = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
    s_fill_go = xSemaphoreCreateBinary();
    s_fill_done = xSemaphoreCreateBinary();
    phrases_load();

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
    xTaskCreatePinnedToCore(care_task, "care", 12288, NULL, 4, NULL, 1);        /* TLS, and speech */
    xTaskCreatePinnedToCore(filler_task, "filler", 4096, NULL, 5, NULL, 1);
    if (s_care_wake) ESP_LOGI(TAG, "awake for a reminder");

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
            s_person_at = time(NULL);
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
                /* The conversation's buttons; elsewhere a tap does nothing, so no one ends it by accident. */
                if (phit == UI_HIT_DONE) {
                    s_stop = true;
                    audio_abort();
                    tts_abort();
                } else if (phit == UI_HIT_AGAIN || phit == UI_HIT_SLOWER || phit == UI_HIT_YES || phit == UI_HIT_NO) {
                    s_action = phit == UI_HIT_AGAIN ? ACT_AGAIN : phit == UI_HIT_SLOWER ? ACT_SLOWER
                             : phit == UI_HIT_YES ? ACT_YES : ACT_NO;
                    audio_abort();
                }
            } else if (m == UI_MESSAGE || m == UI_REMINDER) {
                tts_abort();                                        /* OK, or anywhere: put it away */
                set_view(UI_HOME, NULL, NULL, NULL, NULL);
            } else if (m != UI_SETTINGS && m != UI_EASY && m != UI_VOICES && m != UI_LISTENING && m != UI_THINKING && m != UI_SPEAKING) {
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
                } else if (!moved && phit == UI_HIT_EASY) {
                    set_view(UI_EASY, NULL, NULL, NULL, NULL);
                }
            } else if (m == UI_EASY) {
                if (phit == UI_HIT_BACK) {
                    set_view(UI_SETTINGS, NULL, NULL, NULL, NULL);
                } else if (phit == UI_HIT_TEXTSIZE) {
                    s_textsize = pval;
                    cfg_set(CFG_TEXTSIZE, pval ? "1" : "0");
                } else if (phit == UI_HIT_CONTRAST) {
                    s_contrast = pval;
                    cfg_set(CFG_CONTRAST, pval ? "1" : "0");
                } else if (phit == UI_HIT_CLARITY) {
                    s_clarity = pval;
                    cfg_set(CFG_CLARITY, pval ? "1" : "0");
                    if (s_cur >= 0) job(s_cur);                     /* hear the difference */
                }
                s_dirty = true;
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
            s_person_at = time(NULL);
            if (s_convo) {                                          /* BOOT ends a conversation, whatever the screen */
                s_stop = true;
                audio_abort();
                tts_abort();
            } else if (awake) {
                xSemaphoreGive(s_go);
            }
            activity();
        }
        {
            /* A knock wakes it; two knocks start a conversation. A finger on the
               glass and its own speaker jolt the panel too: those are hushed. */
            long ms = (long)(esp_timer_get_time() / 1000);
            if (now_down || s_in_use || s_mic) knock_hush(&knock, ms + 400);
            float ax, ay, az;
            if (motion_read(&ax, &ay, &az) && knock_feed(&knock, ax, ay, az, ms)) {
                activity();
                s_person_at = time(NULL);
                if (knock_double(&knock, ms) && !s_convo && !s_in_use
                    && (s_view.mode == UI_HOME || s_view.mode == UI_ANSWER || !awake))
                    xSemaphoreGive(s_go);
            }
        }
        down = now_down;
        boot_was = boot_now;

        static bool was_up;
        if (net_up() != was_up) { was_up = net_up(); s_dirty = true; }   /* the network name at the top */
        static unsigned tick;
        if (s_view.mode == UI_LISTENING && (++tick & 1)) s_dirty = true;   /* the waveform and timer move, ~16 fps */
        if (s_view.mode == UI_SPEAKING && s_captions && (++tick & 1)) {     /* the captions follow the voice */
            int len = (int)strlen(s_text), at = (int)(tts_played() / (float)AUDIO_FS * s_cap_rate);
            xSemaphoreTake(s_ui_lock, portMAX_DELAY);
            s_view.spoken = at < len ? at : len;
            s_dirty = true;
            xSemaphoreGive(s_ui_lock);
        }
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
        if ((s_view.mode == UI_SETTINGS || s_view.mode == UI_VOICES || s_view.mode == UI_EASY)
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
            if (m != last_min) {
                last_min = m;
                s_view.now = m;
                s_view.wday = lt.tm_wday;
                s_view.month = lt.tm_mon + 1;
                s_view.mday = lt.tm_mday;
                s_dirty = true;
            }
        }
        /* A fresh time from the network goes into the clock chip. */
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) clockchip_set(time(NULL));

        /* How awake to be, and the light for it. */
        int64_t now_us = esp_timer_get_time();
        int idle_s = s_in_use || s_mic ? 0 : (int)((now_us - s_active_at) / 1000000);
        if (!ALWAYS_ON && net_time_ok() && !office_now() && idle_s >= REST_BREATHE_S) deep_sleep_now(true);
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
        /* While it breathes, a family photo instead of the orb, a new one each
           minute, at a steady light (a photo at a breath's 3-14 % is black). */
        static int photo_i = -1, photo_w, photo_h;
        static int64_t photo_at;
        static const char *photo_from;
        bool photo = want != REST_AWAKE && !amber && s_rest_img;
        if (photo && (photo_i < 0 || now_us - photo_at > 60LL * 1000000)) {
            care_list_t *pl = care_photos();
            photo_at = now_us;
            int tries = pl ? pl->n : 0;
            int was = photo_i;
            photo_i = -1;
            for (int k = 1; k <= tries; k++) {
                int i = ((was < 0 ? -1 : was) + k) % pl->n;
                if (!pl->item[i].done) continue;
                unsigned char *jpeg = NULL;
                int len = care_photo_load(pl->item[i].id, &jpeg);
                bool ok = len > 0 && decode_jpeg(jpeg, (size_t)len, s_rest_img, &photo_w, &photo_h);
                free(jpeg);
                if (ok) { photo_i = i; photo_from = pl->item[i].from; s_dirty = true; break; }
            }
        }
        if (!photo && photo_i >= 0) { photo_i = -1; s_dirty = true; }
        /* At rest it stays lit, steady, as a clock and the weather to read from across
           the room: 70 % by day, 25 % from 22:00 to 07:00. */
        int m = net_time_ok() ? s_view.now : 12 * 60;
        int steady = m >= 22 * 60 || m < 7 * 60 ? 25 : 70;
        (void)rest_t0;
        light(amber ? 60 : want == REST_AWAKE ? 100 : steady);

        if (s_dirty) {
            xSemaphoreTake(s_ui_lock, portMAX_DELAY);
            s_dirty = false;
            s_view.mic_open = s_mic;
            s_view.ssid = net_up() ? net_ssid() : NULL;
            s_view.levels = s_levels;
            s_view.nlevels = s_nlevels;
            s_view.listen_ms = (int)((now_us - s_listen_t0) / 1000);
            if (resting_screen) {
                ui_view_t rv = { .mode = UI_RESTING, .now = s_view.now, .amber = amber, .mic_open = s_mic,
                                 .wday = s_view.wday, .month = s_view.month, .mday = s_view.mday, .spoken = -1 };
                rv.weather = &s_weather;
                if (photo_i >= 0) {
                    rv.image = s_rest_img;
                    rv.img_w = photo_w;
                    rv.img_h = photo_h;
                    rv.photo_from = photo_from;
                }
                ui_draw(lcd_canvas(), &rv);
            } else {
                ui_draw(lcd_canvas(), &s_view);
            }
            lcd_show();
            xSemaphoreGive(s_ui_lock);
        }
    }
}
