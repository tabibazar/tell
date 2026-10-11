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
static care_list_t *s_photos;          /* PSRAM: the photo frame (done = the JPEG is in /mem) */
static char *s_text;                   /* PSRAM: a tool's reply text */
static char *s_media;                  /* PSRAM: a photo's reply (base64 JSON) */
static unsigned char *s_jpeg;          /* PSRAM: a photo, decoded from base64 */
static int s_checkin = -1;             /* the homeowner agreed to the morning check-in: 1, 0, -1 not known */
#define TEXT_MAX  8192
#define MEDIA_MAX (400 * 1024)
#define JPEG_MAX  (160 * 1024)
#define PHOTOS_ON_FLASH 6              /* at most ~1 MB of the 2 MB memory partition */

void care_net_load(void)
{
    if (!s_rem) {
        s_rem = heap_caps_calloc(1, sizeof *s_rem, MALLOC_CAP_SPIRAM);
        s_msg = heap_caps_calloc(1, sizeof *s_msg, MALLOC_CAP_SPIRAM);
        s_photos = heap_caps_calloc(1, sizeof *s_photos, MALLOC_CAP_SPIRAM);
        s_text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    }
    char *t = memory_read("remind.jsn", 16384);
    if (!t || !care_list_load(t, s_rem)) s_rem->n = 0;
    free(t);
    t = memory_read("msgs.jsn", 16384);
    if (!t || !care_list_load(t, s_msg)) s_msg->n = 0;
    free(t);
    t = memory_read("photos.jsn", 16384);
    if (!t || !care_list_load(t, s_photos)) s_photos->n = 0;
    free(t);
    t = memory_read("checkin.txt", 8);
    s_checkin = t ? t[0] == '1' : -1;
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
    t = care_list_json(s_photos);
    if (t) { memory_write("photos.jsn", t); free(t); }
}

bool care_net_ready(void)
{
    return cfg_has(CFG_MCP_URL) && cfg_has(CFG_MCP_TOKEN) && cfg_has(CFG_HW_TOKEN);
}

/* tools/call `tool` for the homeowner, the result's text into out. */
static bool call_max(const char *tool, const char *extra, char *out, size_t n, size_t max)
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
    int code = https_post(url, h, 2, "application/json", body, strlen(body), 20000, &resp, max, NULL, NULL);
    free(body);
    bool ok = code == 200 && resp && care_rpc_text(resp, out, n);
    free(resp);
    if (!ok) ESP_LOGW(TAG, "%s: %d", tool, code);
    return ok;
}

static bool call(const char *tool, const char *extra, char *out, size_t n)
{
    return call_max(tool, extra, out, n, 16384);
}

bool care_net_refresh_reminders(void)
{
    if (!s_text || !call("reminders", NULL, s_text, TEXT_MAX)) return false;
    int n = care_parse_reminders(s_text, s_rem);
    if (n < 0) return false;
    int c = care_parse_checkin(s_text);
    if (c >= 0 && c != s_checkin) { s_checkin = c; memory_write("checkin.txt", c ? "1" : "0"); }
    care_net_save();
    ESP_LOGI(TAG, "%d reminders, morning check-in %s", n, s_checkin == 1 ? "on" : "off");
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

bool care_checkin_on(void) { return s_checkin == 1; }

bool care_net_checkin_missed(const char *asked)
{
    char extra[96];
    snprintf(extra, sizeof extra, ",\"asked\":\"%.60s\"", asked);
    return call("checkin_missed", extra, s_text, TEXT_MAX);
}

/* "p" and the first seven of the id: an 8.3 name. */
static void photo_file(const char *id, char *out, size_t n) { snprintf(out, n, "p%.7s.jpg", id); }

int care_net_sync_photos(void)
{
    if (!s_text || !call("family_photos", NULL, s_text, TEXT_MAX)) return -1;
    care_list_t before = *s_photos;
    if (care_parse_photos(s_text, s_photos) < 0) return -1;
    char name[16];
    for (int i = 0; i < before.n; i++) {                /* gone from the frame: gone from flash */
        bool kept = false;
        for (int k = s_photos->n > PHOTOS_ON_FLASH ? s_photos->n - PHOTOS_ON_FLASH : 0; k < s_photos->n; k++)
            kept |= strcmp(before.item[i].id, s_photos->item[k].id) == 0;
        if (!kept && before.item[i].done) { photo_file(before.item[i].id, name, sizeof name); memory_remove(name); }
    }
    if (!s_media) s_media = heap_caps_malloc(MEDIA_MAX, MALLOC_CAP_SPIRAM);
    if (!s_jpeg) s_jpeg = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM);
    int got = 0;
    for (int i = 0; i < s_photos->n && s_media && s_jpeg; i++) {
        care_item_t *p = &s_photos->item[i];
        if (i < s_photos->n - PHOTOS_ON_FLASH) { p->done = false; continue; }   /* older than the newest six */
        if (p->done) continue;
        char extra[64];
        snprintf(extra, sizeof extra, ",\"id\":\"%.20s\"", p->id);
        if (!call_max("family_media", extra, s_media, MEDIA_MAX, MEDIA_MAX)) break;
        int n = care_media_bytes(s_media, s_jpeg, JPEG_MAX);
        photo_file(p->id, name, sizeof name);
        if (n > 0 && memory_write_bin(name, s_jpeg, (size_t)n)) { p->done = true; got++; }
    }
    care_net_save();
    if (got) ESP_LOGI(TAG, "%d new photos", got);
    return got;
}

care_list_t *care_photos(void) { return s_photos; }

int care_photo_load(const char *id, unsigned char **jpeg)
{
    char name[16];
    photo_file(id, name, sizeof name);
    return memory_read_bin(name, jpeg, JPEG_MAX);
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
    printf("photos (%d):", s_photos->n);
    for (int i = 0; i < s_photos->n; i++) printf(" %s%s", s_photos->item[i].id, s_photos->item[i].done ? "" : "(fetching)");
    printf("\nmorning check-in: %s\n", s_checkin == 1 ? "on" : s_checkin == 0 ? "off" : "not known yet");
    printf("homeowner token: %s\n", cfg_has(CFG_HW_TOKEN) ? "set" : "not set (sign in by voice once, or: hoa token ...)");
}
