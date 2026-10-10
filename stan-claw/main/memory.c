#include "memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "memory";
#define BASE      "/mem"
#define F_CONVO   BASE "/convo.jsn"
#define F_FACTS   BASE "/facts.txt"
#define F_LOG     BASE "/log.txt"
#define F_LOG_OLD BASE "/log.old"
#define LOG_MAX   (512 * 1024)

static bool s_ok;

esp_err_t memory_init(void)
{
    esp_vfs_fat_mount_config_t mc = { .format_if_mount_failed = true, .max_files = 4, .allocation_unit_size = 4096 };
    static wl_handle_t wl;
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(BASE, "memory", &mc, &wl);
    s_ok = e == ESP_OK;
    if (s_ok) ESP_LOGI(TAG, "memory at " BASE);
    else ESP_LOGE(TAG, "memory partition would not mount: %s (it will forget on restart)", esp_err_to_name(e));
    return e;
}

static char *slurp(const char *path, size_t max)
{
    FILE *f = s_ok ? fopen(path, "r") : NULL;
    if (!f) return NULL;
    char *buf = heap_caps_malloc(max + 1, MALLOC_CAP_SPIRAM);
    size_t n = buf ? fread(buf, 1, max, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    return buf;
}

/* Written to a temporary name and renamed, so a power cut mid-write leaves
   the old file, not half a new one. */
static void put(const char *path, const char *text)
{
    if (!s_ok) return;
    const char *tmp = BASE "/tmp.new";            /* 8.3 names: FAT without long names */
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    bool ok = fputs(text, f) >= 0;
    ok = fclose(f) == 0 && ok;
    if (!ok) { remove(tmp); return; }
    remove(path);
    rename(tmp, path);
}

bool memory_load_history(claude_history_t *h, int *day)
{
    char *s = slurp(F_CONVO, 16 * 1024);
    bool ok = s && claude_history_parse(s, h, day);
    free(s);
    return ok;
}

void memory_save_history(const claude_history_t *h, int day)
{
    char *s = claude_history_json(h, day);
    if (s) put(F_CONVO, s);
    free(s);
}

void memory_load_facts(claude_facts_t *f)
{
    char *s = slurp(F_FACTS, FACTS_MAX * FACT_LEN);
    if (s) claude_facts_parse(s, f);
    else f->n = 0;
    free(s);
}

void memory_save_facts(const claude_facts_t *f)
{
    char *s = heap_caps_malloc(FACTS_MAX * FACT_LEN + 1, MALLOC_CAP_SPIRAM);
    if (!s) return;
    claude_facts_text(f, s, FACTS_MAX * FACT_LEN + 1);
    put(F_FACTS, s);
    free(s);
}

void memory_log(const char *q, const char *a)
{
    if (!s_ok) return;
    struct stat st;
    if (stat(F_LOG, &st) == 0 && st.st_size > LOG_MAX) {
        remove(F_LOG_OLD);
        rename(F_LOG, F_LOG_OLD);
    }
    FILE *f = fopen(F_LOG, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    fprintf(f, "%04d-%02d-%02d %02d:%02d\tQ: %s\tA: %s\n", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
            lt.tm_hour, lt.tm_min, q, a);
    fclose(f);
}

void memory_print_log(int last)
{
    char *s = slurp(F_LOG, LOG_MAX + 4096);
    if (!s) { printf("(no conversations yet)\n"); return; }
    /* Walk back `last` lines from the end. */
    char *p = s + strlen(s);
    int lines = 0;
    while (p > s) {
        p--;
        if (*p == '\n' && p[1] && ++lines > last) { p++; break; }
    }
    printf("%s", p);
    free(s);
}

void memory_forget(void)
{
    if (!s_ok) return;
    remove(F_CONVO);
    remove(F_FACTS);
}
