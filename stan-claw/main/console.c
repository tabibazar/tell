#include "console.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "cmdline.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "voices.h"
#include <strings.h>

#define TONE_N (AUDIO_FS / 2)      /* half a second of 440 Hz: 220 whole cycles, so it loops cleanly */

/* The beep, made once in PSRAM (internal RAM is for WiFi and TLS). */
static const int16_t *tone(void)
{
    static int16_t *t;
    if (t) return t;
    t = heap_caps_malloc(TONE_N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (t) for (int i = 0; i < TONE_N; i++) t[i] = (int16_t)(8000 * sinf(2 * 3.14159f * 440 * i / AUDIO_FS));
    return t;
}

static void yes_no(const char *what, cfg_key_t k) { printf("%s: %s\n", what, cfg_has(k) ? "set" : "not set"); }

static void run(const char *line)
{
    static char w[CMD_MAX][CMD_WORD];
    int n = cmd_split(line, w, CMD_MAX);
    if (n == 0) return;
    if (strcmp(w[0], "wifi") == 0 && n >= 2) {
        if (strcmp(w[1], "add") == 0 && n >= 3) {
            if (cfg_net_add(w[2], n >= 4 ? w[3] : "")) { printf("ok: %s known\n", w[2]); net_rejoin(); }
            else printf("no: list full (%d) or name too long\n", CFG_NETS);
            return;
        }
        if (strcmp(w[1], "forget") == 0 && n >= 3) {
            printf(cfg_net_forget(w[2]) ? "ok: %s forgotten\n" : "no: %s was not known\n", w[2]);
            if (strcmp(w[2], net_ssid()) == 0) net_rejoin();
            return;
        }
        if (strcmp(w[1], "list") == 0) {
            static cfg_net_t nets[CFG_NETS];
            int k = cfg_nets(nets);
            for (int i = 0; i < k; i++)
                printf("  %s%s\n", nets[i].ssid, strcmp(nets[i].ssid, net_ssid()) == 0 ? "   <- on it" : "");
            printf("%d known\n", k);
            return;
        }
    }
    if (strcmp(w[0], "key") == 0 && n >= 3) {
        cfg_key_t k = strcmp(w[1], "claude") == 0 ? CFG_CLAUDE_KEY : strcmp(w[1], "deepgram") == 0 ? CFG_DEEPGRAM_KEY
                    : strcmp(w[1], "elevenlabs") == 0 ? CFG_ELEVEN_KEY : CFG_N;
        if (k == CFG_N) { printf("no: key claude|deepgram|elevenlabs KEY\n"); return; }
        cfg_set(k, w[2]);
        printf("ok: %s key set\n", w[1]);
        return;
    }
    if (strcmp(w[0], "voice") == 0 && n >= 2) {
        /* A name from the wheel (voice jarvis) sets the voice and its effect;
           anything else is taken as an ElevenLabs voice id. */
        for (int i = 0; i < VOICES_N; i++) {
            if (strcasecmp(w[1], VOICES[i].name) == 0) {
                cfg_set(CFG_VOICE, VOICES[i].id);
                cfg_set(CFG_VOICE_FX, fx_name(VOICES[i].fx));
                cfg_set(CFG_VOICE_NAME, VOICES[i].name);
                settings_reload();
                printf("ok: voice %s\n", VOICES[i].name);
                return;
            }
        }
        cfg_set(CFG_VOICE, w[1]);
        cfg_set(CFG_VOICE_FX, "");
        cfg_set(CFG_VOICE_NAME, "");
        settings_reload();
        printf("ok: voice id set\n");
        return;
    }
    if (strcmp(w[0], "model") == 0 && n >= 2) { cfg_set(CFG_MODEL, w[1]); printf("ok: model %s\n", w[1]); return; }
    if (strcmp(w[0], "mcp") == 0 && n >= 3 && strcmp(w[1], "url") == 0) {
        if (strncmp(w[2], "https://", 8) != 0 && w[2][0]) { printf("no: an https:// URL (or \"\" for none)\n"); return; }
        cfg_set(CFG_MCP_URL, w[2]);
        printf("ok: mcp url %s\n", w[2][0] ? "set" : "cleared");
        return;
    }
    if (strcmp(w[0], "mcp") == 0 && n >= 3 && strcmp(w[1], "token") == 0) { cfg_set(CFG_MCP_TOKEN, w[2]); printf("ok: mcp token set\n"); return; }
    if (strcmp(w[0], "serve") == 0 && n >= 3 && strcmp(w[1], "token") == 0) { cfg_set(CFG_SERVE_TOKEN, w[2]); printf("ok: serve token set\n"); return; }
    if (strcmp(w[0], "status") == 0) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) esp_netif_get_ip_info(nif, &ip);
        printf("wifi: %s", net_up() ? net_ssid() : "not joined");
        if (net_up()) printf(" as " IPSTR, IP2STR(&ip.ip));
        printf("\nclock: %s\n", net_time_ok() ? "set" : "not set");
        yes_no("claude key", CFG_CLAUDE_KEY);
        yes_no("deepgram key", CFG_DEEPGRAM_KEY);
        yes_no("elevenlabs key", CFG_ELEVEN_KEY);
        yes_no("voice", CFG_VOICE);
        char v[CFG_VAL];
        cfg_get(CFG_MODEL, v, sizeof v);
        printf("model: %s\n", v);
        cfg_get(CFG_MCP_URL, v, sizeof v);
        printf("mcp url: %.60s%s\n", v[0] ? v : "none", strlen(v) > 60 ? "..." : "");
        yes_no("mcp token", CFG_MCP_TOKEN);
        yes_no("serve token", CFG_SERVE_TOKEN);
        return;
    }
    if (strcmp(w[0], "beep") == 0) {
        const int16_t *t = tone();
        if (!t) { printf("no: out of memory\n"); return; }
        audio_play_begin();
        audio_play_chunk(t, TONE_N);
        audio_play_end();
        printf("ok: beeped\n");
        return;
    }
    if (strcmp(w[0], "slots") == 0) {        /* each TDM slot's level over 2 s; "slots beep" plays the tone meanwhile */
        bool beep = n >= 2 && strcmp(w[1], "beep") == 0;
        const int16_t *t = beep ? tone() : NULL;
        if (beep && !t) { printf("no: out of memory\n"); return; }
        int peak[4], rms[4], loud[4];
        int k = audio_slot_levels(2000, t, TONE_N, peak, rms, loud);
        if (k <= 0) { printf("no: the mics are not up\n"); return; }
        for (int c = 0; c < 4; c++) printf("  slot %d: peak %5d  rms %5d  loudest 20 ms %5d\n", c, peak[c], rms[c], loud[c]);
        printf("ok: %d ms of slots%s\n", k * 1000 / AUDIO_FS, beep ? " with the beep" : "");
        return;
    }
    if (strcmp(w[0], "regs") == 0) {         /* the ES7210's setup, as it stands */
        static const int regs[] = { 0x00, 0x02, 0x12, 0x43, 0x44, 0x45, 0x46, 0x4B, 0x4C };
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
            int v = audio_mic_reg(regs[i]);
            if (v < 0) printf("  %02X: read failed\n", regs[i]);
            else printf("  %02X: %02X\n", regs[i], v);
        }
        printf("ok: ES7210 registers\n");
        return;
    }
    if (strcmp(w[0], "rec") == 0) {
        static int16_t *pcm;
        if (!pcm) pcm = heap_caps_malloc(AUDIO_FS * 5 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!pcm) { printf("no: out of memory\n"); return; }
        vad_result_t why;
        int n = audio_record(pcm, AUDIO_FS * 5, 5000, &why);
        double peak = 0;
        for (int i = 0; i < n; i++) peak = fabs((double)pcm[i]) > peak ? fabs((double)pcm[i]) : peak;
        printf("ok: %d ms, peak %.0f, %s\n", n * 1000 / AUDIO_FS, peak,
               why == VAD_DONE ? "speech then quiet" : why == VAD_CAPPED ? "capped" : "silent");
        return;
    }
    printf("commands: wifi add|forget|list, key claude|deepgram|elevenlabs K, voice NAME|ID, model ID,\n"
           "          mcp url URL, mcp token T, serve token T, beep, rec, status,\n"
           "          slots [beep], regs (developer tools)\n");
}

static void console_task(void *arg)
{
    static char line[CMD_WORD * 2];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n) run(line);
            n = 0;
        } else if (n + 1 < sizeof line) {
            line[n++] = (char)c;
        }
    }
}

void console_start(void)
{
    uart_driver_install(UART_NUM_0, 2048, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);
}
