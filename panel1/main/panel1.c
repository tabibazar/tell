/*
 * panel1: the meeting rooms' bookings, one day at a time, on a Waveshare
 * ESP32-S3-Touch-LCD-4B (480x480 ST7701 RGB panel, GT911 touch).
 *
 * The day comes from the relay (panel1/relay/rooms.gs) over WiFi: today every
 * five minutes, any other day when it is looked at. It opens on today -- or,
 * after 17:00 and at weekends, on the next working day -- and a swipe left or
 * right moves a working day, passing over Saturday and Sunday; a minute
 * untouched brings it home again. It shows a row per room, the day running
 * across; a tap on a row opens that room's own page, where a tap on a booking
 * opens a card with all of it -- who booked it, who is invited. Swipe up and
 * down there for the other rooms, and tap its top to come back. Five minutes untouched, the screen goes dark; BOOT, PWR or
 * a tap wakes it on the home day.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "config.h"
#include "console.h"
#include "lcd.h"
#include "net.h"
#include "rooms.h"
#include "roomsui.h"
#include "touch.h"

static const char *TAG = "panel1";

#define TZ_HOME        "EST5EDT,M3.2.0,M11.1.0"
#define REFRESH_TODAY  (5 * 60)     /* s */
#define REFRESH_OTHER  (15 * 60)
#define RETRY          30           /* s after a failed fetch */
#define IDLE_HOME      60           /* s untouched before going home */
#define EVENING        17           /* from this hour the home day is the next */
#define SWIPE_PX       60
#define SPAN           14           /* days either way a swipe may go */
#define CACHE_N        8
#define DETAIL_FOR     20           /* s a booking's card stays up untouched */
#define SLEEP_AFTER    (5 * 60)     /* s untouched before the screen goes dark */
#define PIN_BOOT       0            /* the side BOOT key, low while held */
#define NOTE_AFTER     (3 * 60)     /* s of failed fetches before the header says so */

typedef struct {
    int y, m, d;          /* 0 = empty slot */
    int64_t fetched;      /* s, monotonic; when it last came in */
    int64_t tried;        /* when it was last asked for */
    rooms_day_t day;
} slot_t;

static slot_t *s_cache;               /* PSRAM, CACHE_N slots */
static SemaphoreHandle_t s_lock;
static volatile int s_offset;         /* the day shown, from today */
static volatile int s_room = -1;      /* the open room's page; -1 the overview */
static volatile bool s_dirty = true;
static volatile int64_t s_last_ok;    /* s, monotonic; last good fetch */
static volatile time_t s_last_ok_wall;
static volatile bool s_failing;
static volatile int64_t s_fail_since;  /* s, monotonic; when the failing began */
static bool s_asleep;

/* The booking whose card is up, copied: the day under it may be refetched. */
static bool s_det;
static room_t *s_det_room;          /* PSRAM */
static room_ev_t s_det_ev;
static int64_t s_det_at;
static bool s_buf_ok;               /* the last draw had a day to show */

static int64_t now_s(void) { return esp_timer_get_time() / 1000000; }

/* The local date `off` days from today. */
static struct tm day_at(int off)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_mday += off;
    lt.tm_hour = 12;    /* noon: a DST change cannot push it into the next day */
    lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    t = mktime(&lt);
    localtime_r(&t, &lt);
    return lt;
}

static bool weekend(int off)
{
    int wd = day_at(off).tm_wday;
    return wd == 0 || wd == 6;
}

/* Today in working hours; after EVENING, or at a weekend, the next working day. */
static int home_offset(void)
{
    time_t t = time(NULL);
    struct tm lt;
    localtime_r(&t, &lt);
    int off = lt.tm_hour >= EVENING ? 1 : 0;
    for (;;) {
        int wd = (lt.tm_wday + off) % 7;
        if (wd != 0 && wd != 6) return off;
        off++;
    }
}

/* The overview on the home day, no card open. */
static void go_home(void)
{
    if (net_time_ok()) s_offset = home_offset();
    s_room = -1;
    s_det = false;
    s_dirty = true;
}

static slot_t *find(const struct tm *t)
{
    for (int i = 0; i < CACHE_N; i++)
        if (s_cache[i].y == t->tm_year + 1900 && s_cache[i].m == t->tm_mon + 1 && s_cache[i].d == t->tm_mday)
            return &s_cache[i];
    return NULL;
}

/* A slot for `t`: its own, or an empty one, or the one fetched longest ago. */
static slot_t *claim(const struct tm *t)
{
    slot_t *s = find(t);
    if (s) return s;
    slot_t *old = &s_cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        if (!s_cache[i].y) { old = &s_cache[i]; break; }
        if (s_cache[i].tried < old->tried) old = &s_cache[i];
    }
    memset(old, 0, sizeof *old);
    old->y = t->tm_year + 1900;
    old->m = t->tm_mon + 1;
    old->d = t->tm_mday;
    return old;
}

/* Fetches the day `off` from today if it is due. True when it asked. */
static bool fetch_if_due(int off, int every, rooms_day_t *tmp)
{
    struct tm t = day_at(off);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    slot_t *s = claim(&t);
    int64_t n = now_s();
    bool failed_last = s->tried > s->fetched;   /* a good fetch stamps after its try */
    bool due = !(s->fetched && n - s->fetched < every) && !(failed_last && n - s->tried < RETRY);
    if (due) s->tried = n;
    xSemaphoreGive(s_lock);
    if (!due) return false;

    bool ok = net_fetch_day(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, tmp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = claim(&t);
    if (ok) {
        s->day = *tmp;
        s->fetched = now_s();
        s_last_ok = s->fetched;
        s_last_ok_wall = time(NULL);
    }
    if (!ok && !s_failing) s_fail_since = now_s();
    s_failing = !ok;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return true;
}

void panel1_refetch(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_cache, 0, CACHE_N * sizeof *s_cache);
    s_failing = false;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

static void fetch_task(void *arg)
{
    rooms_day_t *tmp = heap_caps_malloc(sizeof *tmp, MALLOC_CAP_SPIRAM);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!net_up() || !net_time_ok() || !net_have_relay()) continue;
        /* What is on screen first, then today (for the header's free/busy),
           then the home day, so a swipe back is instant. */
        if (fetch_if_due(s_offset, s_offset == 0 ? REFRESH_TODAY : REFRESH_OTHER, tmp)) continue;
        if (fetch_if_due(0, REFRESH_TODAY, tmp)) continue;
        int h = home_offset();
        if (h != 0) fetch_if_due(h, REFRESH_OTHER, tmp);
    }
}

static void draw(canvas_t *c, rooms_day_t *buf)
{
    roomsui_view_t v = { .offset = s_offset, .now = -1 };
    char note[40] = "";
    const rooms_day_t *day = NULL;

    if (!net_time_ok()) {
        snprintf(note, sizeof note, "%s", net_up() ? "setting the clock" : "joining WiFi");
    } else {
        time_t t = time(NULL);
        struct tm lt;
        localtime_r(&t, &lt);
        v.now = lt.tm_hour * 60 + lt.tm_min;
        struct tm d = day_at(s_offset);
        v.wday = d.tm_wday;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_t *s = find(&d);
        if (s && s->fetched) {
            *buf = s->day;
            day = buf;
        }
        s_buf_ok = day != NULL;
        /* Apps Script stalls now and then; say so only when it keeps on. */
        bool failing = s_failing && now_s() - s_fail_since >= NOTE_AFTER;
        xSemaphoreGive(s_lock);
        if (!net_have_relay()) {
            snprintf(note, sizeof note, "no relay URL");
        } else if (!net_up()) {
            snprintf(note, sizeof note, "no WiFi");
        } else if (failing && s_last_ok_wall) {
            struct tm ok;
            time_t w = s_last_ok_wall;
            localtime_r(&w, &ok);
            snprintf(note, sizeof note, "offline %d:%02d", ok.tm_hour, ok.tm_min);
        } else if (failing) {
            snprintf(note, sizeof note, "relay not answering");
        }
    }
    v.note = note;
    if (s_room >= 0 && day && s_room >= day->nrooms) s_room = -1;   /* that day has fewer rooms */
    roomsui_room_draw(c, day, s_room, &v);
    if (s_det && v.now >= 0) roomsui_detail(c, s_det_room, &s_det_ev, &v);
    lcd_show();
}

/* Dark: the frame black, then the backlight off, so nothing glows through. */
static void sleep_now(canvas_t *c)
{
    s_asleep = true;
    s_det = false;
    canvas_fill_rect(c, 0, 0, c->w, c->h, 0);
    lcd_show();
    lcd_backlight(0);
    ESP_LOGI(TAG, "asleep");
}

/* Back on the overview, home day, at full light. */
static void wake(canvas_t *c, rooms_day_t *buf)
{
    s_asleep = false;
    go_home();
    draw(c, buf);
    s_dirty = false;
    lcd_backlight(100);
    ESP_LOGI(TAG, "awake");
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    setenv("TZ", TZ_HOME, 1);
    tzset();
    cfg_load();

    s_lock = xSemaphoreCreateMutex();
    s_cache = heap_caps_calloc(CACHE_N, sizeof *s_cache, MALLOC_CAP_SPIRAM);
    rooms_day_t *buf = heap_caps_malloc(sizeof *buf, MALLOC_CAP_SPIRAM);
    s_det_room = heap_caps_malloc(sizeof *s_det_room, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(lcd_init());
    canvas_t *c = lcd_canvas();
    bool touch = touch_init() == ESP_OK;
    if (!touch) ESP_LOGW(TAG, "no touch: today only");
    draw(c, buf);

    net_start();
    console_start();
    xTaskCreatePinnedToCore(fetch_task, "fetch", 8192, NULL, 4, NULL, 0);

    gpio_config_t boot = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&boot);
    bool keys_were = false, pwr_was = false, boot_was = false;

    bool down = false, moved = false, waking = false;
    int x0 = 0, y0 = 0, lx = 0, ly = 0;
    int64_t last_touch = now_s();
    int last_min = -1, last_home = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));

        /* The side keys: a press wakes it, or, awake, goes home. Taken on the
           press, not the release. */
        bool pwr = lcd_pwr_key(), bootk = gpio_get_level(PIN_BOOT) == 0;
        if (pwr != pwr_was) { ESP_LOGI(TAG, "PWR key %s", pwr ? "down" : "up"); pwr_was = pwr; }
        if (bootk != boot_was) { ESP_LOGI(TAG, "BOOT key %s", bootk ? "down" : "up"); boot_was = bootk; }
        bool keys = pwr || bootk;
        if (keys && !keys_were) {
            if (s_asleep) wake(c, buf);
            else go_home();
            last_touch = now_s();
        }
        keys_were = keys;

        int x, y;
        if (touch && touch_read(&x, &y)) {
            if (!down) {
                down = true; moved = false; x0 = x; y0 = y;
                if (s_asleep) {
                    /* This touch only wakes: nothing else till the finger lifts. */
                    waking = true;
                    wake(c, buf);
                }
            }
            lx = x; ly = y;
            if (abs(x - x0) > SWIPE_PX / 2 || abs(y - y0) > SWIPE_PX / 2) moved = true;
            last_touch = now_s();
        } else if (down && waking) {
            down = waking = false;
        } else if (down) {
            down = false;
            int dx = lx - x0, dy = ly - y0;
            if (moved && abs(dx) >= SWIPE_PX && abs(dx) >= abs(dy)) {
                /* A finger dragged left pulls the next day in, as a page turns. */
                int step = dx < 0 ? 1 : -1, off = s_offset + step;
                while (weekend(off)) off += step;       /* Saturday and Sunday are skipped */
                if (off >= -SPAN && off <= SPAN) { s_offset = off; s_dirty = true; }
                s_det = false;
            } else if (moved && abs(dy) >= SWIPE_PX && s_room >= 0 && s_buf_ok && buf->nrooms > 0) {
                /* Dragged up, the next room comes in; round from the last to the first. */
                int n = buf->nrooms;
                s_room = (s_room + (dy < 0 ? 1 : n - 1)) % n;
                s_det = false;
                s_dirty = true;
            } else if (!moved && s_det) {
                /* Any tap closes a booking's card. */
                s_det = false;
                s_dirty = true;
            } else if (!moved && s_room >= 0) {
                /* A room page: its top goes back to the overview, a booking opens its card. */
                const room_ev_t *e = s_buf_ok ? roomsui_room_hit(buf, s_room, x0, y0) : NULL;
                if (roomsui_room_head_hit(x0, y0)) {
                    s_room = -1;
                    s_dirty = true;
                } else if (e) {
                    *s_det_room = buf->room[s_room];
                    s_det_ev = *e;
                    s_det = true;
                    s_det_at = now_s();
                    s_dirty = true;
                }
            } else if (!moved) {
                /* The overview: a room's row opens its page; anywhere else goes home. */
                int r = s_buf_ok ? roomsui_head_hit(buf, x0, y0) : -1;
                if (r >= 0) {
                    s_room = r;
                    s_dirty = true;
                } else {
                    int h = home_offset();
                    if (s_offset != h) { s_offset = h; s_dirty = true; }
                }
            }
        }

        if (net_time_ok()) {
            int h = home_offset();
            /* The clock just came: start on the home day. */
            if (last_home < 0 && s_offset != h) { s_offset = h; s_dirty = true; }
            /* Home moves at EVENING and at midnight; follow it if we were there. */
            if (last_home >= 0 && h != last_home && s_offset == last_home) { s_offset = h; s_room = -1; s_dirty = true; }
            last_home = h;
            if ((s_offset != h || s_room >= 0) && now_s() - last_touch >= IDLE_HOME && !down) go_home();
            if (s_det && now_s() - s_det_at >= DETAIL_FOR && now_s() - last_touch >= DETAIL_FOR) { s_det = false; s_dirty = true; }
            if (!s_asleep && !down && now_s() - last_touch >= SLEEP_AFTER) sleep_now(c);
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            if (lt.tm_min != last_min) { last_min = lt.tm_min; s_dirty = true; }
        } else {
            static int64_t last_draw;
            if (now_s() - last_draw >= 2) { last_draw = now_s(); s_dirty = true; }
        }
        if (s_dirty && !down && !s_asleep) {
            s_dirty = false;
            draw(c, buf);
        }
    }
}
