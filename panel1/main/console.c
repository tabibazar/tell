#include "console.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "driver/uart.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"

/* The next word of *p into out (n bytes): a run without spaces, or a "quoted"
   run with them. Returns false at the end of the line. */
static bool word(const char **p, char *out, size_t n)
{
    const char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return false;
    size_t o = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"') { if (o + 1 < n) out[o++] = *s; s++; }
        if (*s == '"') s++;
    } else {
        while (*s && *s != ' ' && *s != '\t') { if (o + 1 < n) out[o++] = *s; s++; }
    }
    out[o] = 0;
    *p = s;
    return true;
}

static void run(const char *line)
{
    char cmd[16], a[80], b[80];
    const char *p = line;
    if (!word(&p, cmd, sizeof cmd)) return;
    if (strcmp(cmd, "wifi") == 0 && word(&p, a, sizeof a)) {
        if (strcmp(a, "add") == 0 && word(&p, a, sizeof a)) {
            if (!word(&p, b, sizeof b)) b[0] = 0;      /* an open network */
            if (cfg_net_add(a, b)) {
                printf("ok: %s known%s\n", a, b[0] ? "" : " (open, no password)");
                net_rejoin();
            } else {
                printf("no: the list is full (%d) or the name is too long\n", CFG_NETS);
            }
            return;
        }
        if (strcmp(a, "forget") == 0 && word(&p, a, sizeof a)) {
            printf(cfg_net_forget(a) ? "ok: %s forgotten\n" : "no: %s was not known\n", a);
            if (strcmp(a, net_ssid()) == 0) net_rejoin();
            return;
        }
        if (strcmp(a, "list") == 0) {
            static cfg_net_t nets[CFG_NETS];
            int n = cfg_nets(nets);
            for (int i = 0; i < n; i++)
                printf("  %s%s\n", nets[i].ssid, strcmp(nets[i].ssid, net_ssid()) == 0 ? "   <- on it" : "");
            printf("%d known\n", n);
            return;
        }
    }
    if (strcmp(cmd, "relay") == 0 && word(&p, a, sizeof a)) {
        /* The URL is longer than a[]: take the rest of the line as it is. */
        const char *u = line + (strstr(line, "relay") - line) + 5;
        while (*u == ' ') u++;
        char url[320];
        snprintf(url, sizeof url, "%s", u);
        char *e = url + strlen(url);
        while (e > url && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        if (strncmp(url, "https://", 8) != 0) { printf("no: the relay is an https:// URL\n"); return; }
        cfg_set_relay(url);
        panel1_refetch();
        printf("ok: relay set, fetching from it\n");
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        char u[320];
        cfg_relay(u, sizeof u);
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nif) esp_netif_get_ip_info(nif, &ip);
        printf("wifi: %s", net_up() ? net_ssid() : "not joined");
        if (net_up()) printf(" as " IPSTR, IP2STR(&ip.ip));
        printf("\nclock: %s\nrelay: %.60s%s\n", net_time_ok() ? "set" : "not set", u[0] ? u : "not set",
               strlen(u) > 60 ? "..." : "");
        return;
    }
    printf("commands: wifi add NAME PASSWORD | wifi forget NAME | wifi list | relay URL | status\n");
}

static void console_task(void *arg)
{
    char line[400];
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
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
