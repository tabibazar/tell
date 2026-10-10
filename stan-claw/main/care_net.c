#include "care_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "https.h"
#include "memory.h"

static const char *TAG = "care";

static care_list_t *s_rem, *s_msg;     /* PSRAM: 3 KB each */
static char *s_text;                   /* PSRAM: a tool's reply text */
#define TEXT_MAX 8192

void care_net_load(void)
{
    if (!s_rem) {
        s_rem = heap_caps_calloc(1, sizeof *s_rem, MALLOC_CAP_SPIRAM);
        s_msg = heap_caps_calloc(1, sizeof *s_msg, MALLOC_CAP_SPIRAM);
        s_text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    }
    char *t = memory_read("remind.jsn", 16384);
    if (!t || !care_list_load(t, s_rem)) s_rem->n = 0;
    free(t);
    t = memory_read("msgs.jsn", 16384);
    if (!t || !care_list_load(t, s_msg)) s_msg->n = 0;
    free(t);
}

care_list_t *care_reminders(void) { return s_rem; }
care_list_t *care_messages(void) { return s_msg; }

void care_net_save(void)
{
    char *t = care_list_json(s_rem);
    if (t) { memory_write("remind.jsn", t); free(t); }
    t = care_list_json(s_msg);
    if (t) { memory_write("msgs.jsn", t); free(t); }
}

bool care_net_ready(void)
{
    return cfg_has(CFG_MCP_URL) && cfg_has(CFG_MCP_TOKEN) && cfg_has(CFG_HW_TOKEN);
}

/* tools/call `tool` for the homeowner, the result's text into out. */
static bool call(const char *tool, const char *extra, char *out, size_t n)
{
    char url[CFG_VAL], tok[CFG_VAL], hw[CFG_VAL], auth[CFG_VAL + 8], args[CFG_VAL + 64];
    cfg_get(CFG_MCP_URL, url, sizeof url);
    cfg_get(CFG_MCP_TOKEN, tok, sizeof tok);
    cfg_get(CFG_HW_TOKEN, hw, sizeof hw);
    if (!url[0] || !tok[0] || !hw[0]) return false;
    snprintf(auth, sizeof auth, "Bearer %s", tok);
    snprintf(args, sizeof args, "{\"homeowner_token\":\"%.60s\"%s}", hw, extra ? extra : "");
    char *body = care_rpc_body(tool, args);
    if (!body) return false;
    https_hdr_t h[] = { { "Authorization", auth }, { "Accept", "application/json, text/event-stream" } };
    char *resp = NULL;
    int code = https_post(url, h, 2, "application/json", body, strlen(body), 15000, &resp, 16384, NULL, NULL);
    free(body);
    bool ok = code == 200 && resp && care_rpc_text(resp, out, n);
    free(resp);
    if (!ok) ESP_LOGW(TAG, "%s: %d", tool, code);
    return ok;
}

bool care_net_refresh_reminders(void)
{
    if (!s_text || !call("reminders", NULL, s_text, TEXT_MAX)) return false;
    int n = care_parse_reminders(s_text, s_rem);
    if (n < 0) return false;
    care_net_save();
    ESP_LOGI(TAG, "%d reminders", n);
    return true;
}

int care_net_check_inbox(void)
{
    if (!s_text || !call("family_inbox", NULL, s_text, TEXT_MAX)) return -1;
    int fresh = care_parse_messages(s_text, s_msg);
    if (fresh > 0) {
        care_net_save();
        ESP_LOGI(TAG, "%d new messages from family", fresh);
    }
    return fresh;
}

int care_next_message(void)
{
    for (int i = 0; i < s_msg->n; i++)
        if (!s_msg->item[i].done) return i;
    return -1;
}

void care_net_print(long now)
{
    printf("reminders (%d):\n", s_rem->n);
    for (int i = 0; i < s_rem->n; i++) {
        time_t at = s_rem->item[i].at;
        struct tm lt;
        localtime_r(&at, &lt);
        printf("  %s %02d-%02d %02d:%02d  %s  %.60s\n", s_rem->item[i].done ? "said" : (at <= now ? "due " : "    "),
               lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, s_rem->item[i].id, s_rem->item[i].text);
    }
    printf("messages (%d):\n", s_msg->n);
    for (int i = 0; i < s_msg->n; i++)
        printf("  %s %s: %.70s\n", s_msg->item[i].done ? "shown" : "new  ", s_msg->item[i].from, s_msg->item[i].text);
    printf("homeowner token: %s\n", cfg_has(CFG_HW_TOKEN) ? "set" : "not set (sign in by voice once, or: hoa token ...)");
}
