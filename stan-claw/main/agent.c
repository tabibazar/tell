#include "agent.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "https.h"
#include "memory.h"

static claude_history_t *s_hist;       /* PSRAM: 12.6 KB internal RAM would rather keep for TLS */
static claude_facts_t *s_facts;        /* PSRAM: what it knows about the person */
static int s_day = -1;                 /* the day (claude_day) the conversation belongs to */

/* One request; Claude 429/529: one retry after 2 s. */
static int post(const char *key, const char *model, const char *url, const char *tok, const char *question,
                claude_reply_t *out)
{
    static char facts[FACTS_MAX * FACT_LEN];
    claude_facts_line(s_facts, facts, sizeof facts);
    char *body = claude_request(model, url, tok, s_hist, facts, question);
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
        /* The person signed in through stan-mcp: keep their token, so the board
           can fetch their reminders and family messages itself. */
        char hw[48], had[CFG_VAL];
        if (code == 200 && resp && claude_find_homeowner_token(resp, hw, sizeof hw)) {
            cfg_get(CFG_HW_TOKEN, had, sizeof had);
            if (strcmp(had, hw) != 0) cfg_set(CFG_HW_TOKEN, hw);
        }
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
    if (!s_hist) {
        /* First question since boot: the conversation and the facts from flash. */
        s_hist = heap_caps_calloc(1, sizeof *s_hist, MALLOC_CAP_SPIRAM);
        s_facts = heap_caps_calloc(1, sizeof *s_facts, MALLOC_CAP_SPIRAM);
        if (!s_hist || !s_facts) { snprintf(out->error, sizeof out->error, "out of memory"); return -1; }
        if (!memory_load_history(s_hist, &s_day)) claude_history_clear(s_hist);
        memory_load_facts(s_facts);
    }
    char key[CFG_VAL], model[CFG_VAL], url[CFG_VAL], tok[CFG_VAL];
    cfg_get(CFG_CLAUDE_KEY, key, sizeof key);
    if (!key[0]) return 0;
    cfg_get(CFG_MODEL, model, sizeof model);
    cfg_get(CFG_MCP_URL, url, sizeof url);
    cfg_get(CFG_MCP_TOKEN, tok, sizeof tok);
    /* A conversation lasts until 04:00 the next day (once the clock is known). */
    time_t now = time(NULL);
    if (now > 1767225600) {
        int today = claude_day(now);
        if (today != s_day) { claude_history_clear(s_hist); s_day = today; }
    }

    int code = post(key, model, url, tok, question, out);
    if (url[0] && claude_mcp_failed(code, out)) {
        /* The remote MCP server failed the whole request: answer without it. */
        code = post(key, model, "", "", question, out);
        out->mcp_error = true;          /* after the parse, which clears the reply */
    }
    if (code == 200) {
        out->end = claude_take_end(out->text);             /* "that's all": Claude closed it */
        claude_take_confirm(out->text, out->confirm, sizeof out->confirm);   /* the Yes / No card */
        if (claude_take_notes(out->text, s_facts) > 0) memory_save_facts(s_facts);
        claude_history_add(s_hist, question, out->text);
        memory_save_history(s_hist, s_day);
        memory_log(question, out->text);
    }
    return code;
}
