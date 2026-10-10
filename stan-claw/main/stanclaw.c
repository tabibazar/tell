/*
 * stan-claw: tap, speak, and it answers aloud and on screen (Deepgram,
 * Claude with a remote MCP server, ElevenLabs); and an MCP server on the
 * local network for its speaker, mics and screen. On a Waveshare
 * ESP32-S3-Touch-LCD-4B. docs/superpowers/specs/2026-10-10-stan-claw-design.md
 *
 * One lock (s_busy) owns the speaker, the mics and the screen: a tap's
 * conversation holds it from the tap to the end of the answer, and each MCP
 * tool call holds it for its own length, waiting up to 30 s for it.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
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
#include "audio.h"
#include "config.h"
#include "console.h"
#include "lcd.h"
#include "mcpd.h"
#include "net.h"
#include "stt.h"
#include "touch.h"
#include "tts.h"
#include "ui.h"

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
static void conversation(void)
{
    char err[64];
    if (!net_up()) { set_view(UI_HOME, NULL, NULL, NULL, "no WiFi"); return; }
    set_view(UI_LISTENING, NULL, NULL, NULL, NULL);
    vad_result_t why;
    int n = audio_record(s_pcm, AUDIO_FS * REC_MAX_S, REC_MAX_S * 1000, &why);
    if (why == VAD_SILENT) { set_view(UI_HOME, NULL, NULL, NULL, "didn't catch that"); return; }

    set_view(UI_THINKING, "...", NULL, NULL, NULL);
    static char heard[600];
    int code = stt_transcribe(s_pcm, n, heard, sizeof heard);
    if (code != 200) { set_view(UI_ERROR, NULL, "Check the key with: tools/stan-claw.py status", service_error("Deepgram", code, err, sizeof err), NULL); return; }
    if (!heard[0]) { set_view(UI_HOME, NULL, NULL, NULL, "didn't catch that"); return; }

    set_view(UI_THINKING, heard, NULL, NULL, NULL);
    static claude_reply_t reply;
    code = agent_ask(heard, &reply);
    if (code != 200) {
        set_view(UI_ERROR, heard, reply.error[0] ? reply.error : "", service_error("Claude", code, err, sizeof err), NULL);
        return;
    }
    char note[96] = "";
    if (reply.mcp_error) snprintf(note, sizeof note, "MCP server unreachable");
    else if (reply.tools[0]) snprintf(note, sizeof note, "asked remote: %.70s", reply.tools);

    set_view(UI_SPEAKING, heard, reply.text, NULL, note);
    code = tts_speak(reply.text);
    s_answer_at = esp_timer_get_time();         /* before UI_ANSWER, so the main loop can't time it out at once */
    if (code != 200) set_view(UI_ANSWER, heard, reply.text, NULL, service_error("ElevenLabs", code, err, sizeof err));
    else set_view(UI_ANSWER, heard, reply.text, NULL, note);
}

static void convo_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        if (xSemaphoreTake(s_busy, 0) != pdTRUE) continue;      /* an agent is using it: ignore the tap */
        conversation();
        ESP_LOGI(TAG, "convo stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreTake(s_go, 0);        /* drop a BOOT press that queued during the conversation */
        xSemaphoreGive(s_busy);
    }
}

/* ---- the MCP tools: each takes s_busy for its own length ---- */

static bool take(char *out, size_t n)
{
    if (xSemaphoreTake(s_busy, pdMS_TO_TICKS(BUSY_WAIT_MS)) == pdTRUE) return true;
    snprintf(out, n, "busy");
    return false;
}

static bool t_speak(const char *text, char *out, size_t n)
{
    if (!take(out, n)) return false;
    ui_mode_t was = s_view.mode;
    if (was == UI_HOME) set_view(UI_SPEAKING, NULL, text, NULL, "an agent is speaking");
    int code = tts_speak(text);
    if (was == UI_HOME) set_view(UI_HOME, NULL, NULL, NULL, NULL);
    xSemaphoreGive(s_busy);
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
    xSemaphoreGive(s_busy);
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
    xSemaphoreGive(s_busy);
    return ok;
}

static bool t_show_text(const char *title, const char *text, char *out, size_t n)
{
    if (!take(out, n)) return false;
    set_view(UI_AGENT_TEXT, NULL, text, title, NULL);
    xSemaphoreGive(s_busy);
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
    xSemaphoreGive(s_busy);
    return ok;
}

static bool t_clear(char *out, size_t n)
{
    if (!take(out, n)) return false;
    set_view(UI_HOME, NULL, NULL, NULL, NULL);
    xSemaphoreGive(s_busy);
    snprintf(out, n, "cleared");
    return true;
}

static bool t_volume(int level, char *out, size_t n)
{
    audio_set_volume(level);
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
    s_busy = xSemaphoreCreateMutex();
    s_ui_lock = xSemaphoreCreateMutex();
    s_go = xSemaphoreCreateBinary();
    s_pcm = heap_caps_malloc(AUDIO_FS * 30 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_image = heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);

    ESP_ERROR_CHECK(lcd_init());
    bool touch = touch_init() == ESP_OK;
    if (audio_init() != ESP_OK) set_view(UI_ERROR, NULL, "The speaker or microphones did not start.", "Audio failed", NULL);
    audio_on_mic(on_mic);
    gpio_config_t boot = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);

    net_start();
    console_start();
    mcpd_start(&TOOLS);
    xTaskCreatePinnedToCore(convo_task, "convo", 16384, NULL, 5, NULL, 1);      /* TLS x3 */

    bool down = false, boot_was = true;
    int last_min = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int x, y;
        bool now_down = touch && touch_read(&x, &y);
        bool boot_now = gpio_get_level(PIN_BOOT) != 0;
        if (now_down && !down) {
            ui_mode_t m = s_view.mode;
            if (m == UI_HOME ? ui_hit_talk(x, y) : (m != UI_LISTENING && m != UI_THINKING && m != UI_SPEAKING)) {
                if (m == UI_HOME) xSemaphoreGive(s_go);
                else set_view(UI_HOME, NULL, NULL, NULL, NULL);          /* a tap clears answers and agent screens */
            }
        }
        if (!boot_now && boot_was) xSemaphoreGive(s_go);
        down = now_down;
        boot_was = boot_now;

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
        if (s_dirty) {
            xSemaphoreTake(s_ui_lock, portMAX_DELAY);
            s_dirty = false;
            s_view.mic_open = s_mic;
            ui_draw(lcd_canvas(), &s_view);
            lcd_show();
            xSemaphoreGive(s_ui_lock);
        }
    }
}
