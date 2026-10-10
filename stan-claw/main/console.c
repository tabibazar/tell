#include "console.h"

#include <stdio.h>
#include <string.h>

#include "cmdline.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

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
    if (strcmp(w[0], "voice") == 0 && n >= 2) { cfg_set(CFG_VOICE, w[1]); printf("ok: voice set\n"); return; }
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
    printf("commands: wifi add|forget|list, key claude|deepgram|elevenlabs K, voice ID, model ID,\n"
           "          mcp url URL, mcp token T, serve token T, status\n");
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
