#include "agent.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "https.h"

#define FORGET_US (2LL * 60 * 1000000)

static claude_history_t *s_hist;       /* PSRAM: 12.6 KB internal RAM would rather keep for TLS */
static int64_t s_last;

/* One request; Claude 429/529: one retry after 2 s. */
static int post(const char *key, const char *model, const char *url, const char *tok, const char *question,
                claude_reply_t *out)
{
    char *body = claude_request(model, url, tok, s_hist, question);
    if (!body) return -1;
    https_hdr_t h[] = {
        { "x-api-key", key },
        { "anthropic-version", "2023-06-01" },
        { "anthropic-beta", "mcp-client-2025-11-20" },
    };
    int nh = url[0] ? 3 : 2;
    int code = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        char *resp = NULL;
        code = https_post("https://api.anthropic.com/v1/messages", h, nh, "application/json",
                          body, strlen(body), 60000, &resp, 65536, NULL, NULL);
        bool parsed = resp && claude_parse(resp, out);
        free(resp);
        if (code == 200 && !parsed) code = -1;
        if ((code == 429 || code == 529) && attempt == 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        break;
    }
    free(body);
    return code;
}

int agent_ask(const char *question, claude_reply_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s_hist) s_hist = heap_caps_calloc(1, sizeof *s_hist, MALLOC_CAP_SPIRAM);
    if (!s_hist) { snprintf(out->error, sizeof out->error, "out of memory"); return -1; }
    char key[CFG_VAL], model[CFG_VAL], url[CFG_VAL], tok[CFG_VAL];
    cfg_get(CFG_CLAUDE_KEY, key, sizeof key);
    if (!key[0]) return 0;
    cfg_get(CFG_MODEL, model, sizeof model);
    cfg_get(CFG_MCP_URL, url, sizeof url);
    cfg_get(CFG_MCP_TOKEN, tok, sizeof tok);
    if (esp_timer_get_time() - s_last > FORGET_US) claude_history_clear(s_hist);

    int code = post(key, model, url, tok, question, out);
    if (url[0] && claude_mcp_failed(code, out)) {
        /* The remote MCP server failed the whole request: answer without it. */
        code = post(key, model, "", "", question, out);
        out->mcp_error = true;          /* after the parse, which clears the reply */
    }
    if (code == 200) {
        claude_history_add(s_hist, question, out->text);
        s_last = esp_timer_get_time();
    }
    return code;
}
