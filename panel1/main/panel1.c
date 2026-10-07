/*
 * panel1: the meeting rooms' bookings, one day at a time, on a Waveshare
 * ESP32-S3-Touch-LCD-4B (480x480 ST7701 RGB panel, GT911 touch).
 *
 * The day comes from the relay (panel1/relay/rooms.gs) over WiFi: today every
 * five minutes, any other day when it is looked at. It opens on today -- or,
 * after 18:00 and at weekends, on the next working day -- and a swipe left or
 * right moves a day; a minute untouched brings it home again. A tap on a
 * booking opens a card with all of it -- who booked it, who is invited.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

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
#define EVENING        18           /* from this hour the home day is the next */
#define SWIPE_PX       60
#define SPAN           14           /* days either way a swipe may go */
#define CACHE_N        8
#define DETAIL_FOR     20           /* s a booking's card stays up untouched */

typedef struct {
    int y, m, d;          /* 0 = empty slot */
    int64_t fetched;      /* s, monotonic; when it last came in */
    int64_t tried;        /* when it was last asked for */
    rooms_day_t day;
} slot_t;

static slot_t *s_cache;               /* PSRAM, CACHE_N slots */
static SemaphoreHandle_t s_lock;
static volatile int s_offset;         /* the day shown, from today */
static volatile bool s_dirty = true;
static volatile int64_t s_last_ok;    /* s, monotonic; last good fetch */
static volatile time_t s_last_ok_wall;
static volatile bool s_failing;

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
    s_failing = !ok;
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return true;
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
        bool failing = s_failing;
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
    roomsui_draw(c, day, &v);
    if (s_det && v.now >= 0) roomsui_detail(c, s_det_room, &s_det_ev, &v);
    lcd_show();
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
    xTaskCreatePinnedToCore(fetch_task, "fetch", 8192, NULL, 4, NULL, 0);

    bool down = false, moved = false;
    int x0 = 0, y0 = 0, lx = 0;
    int64_t last_touch = now_s();
    int last_min = -1, last_home = -1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30));
        int x, y;
        if (touch && touch_read(&x, &y)) {
            if (!down) { down = true; moved = false; x0 = x; y0 = y; }
            lx = x;
            if (abs(x - x0) > SWIPE_PX / 2) moved = true;
            last_touch = now_s();
        } else if (down) {
            down = false;
            int dx = lx - x0;
            /* A finger dragged left pulls the next day in, as a page turns. */
            if (moved && abs(dx) >= SWIPE_PX && abs(dx) > abs(y - y0)) {
                int off = s_offset + (dx < 0 ? 1 : -1);
                if (off >= -SPAN && off <= SPAN) { s_offset = off; s_dirty = true; }
                s_det = false;
            } else if (!moved && s_det) {
                /* Any tap closes a booking's card. */
                s_det = false;
                s_dirty = true;
            } else if (!moved) {
                /* A tap on a booking opens its card; anywhere else goes home. */
                int room;
                const room_ev_t *e = s_buf_ok ? roomsui_hit(buf, x0, y0, &room) : NULL;
                if (e) {
                    *s_det_room = buf->room[room];
                    s_det_ev = *e;
                    s_det = true;
                    s_det_at = now_s();
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
            if (last_home >= 0 && h != last_home && s_offset == last_home) { s_offset = h; s_dirty = true; }
            last_home = h;
            if (s_offset != h && now_s() - last_touch >= IDLE_HOME && !down) { s_offset = h; s_det = false; s_dirty = true; }
            if (s_det && now_s() - s_det_at >= DETAIL_FOR && now_s() - last_touch >= DETAIL_FOR) { s_det = false; s_dirty = true; }
            time_t t = time(NULL);
            struct tm lt;
            localtime_r(&t, &lt);
            if (lt.tm_min != last_min) { last_min = lt.tm_min; s_dirty = true; }
        } else {
            static int64_t last_draw;
            if (now_s() - last_draw >= 2) { last_draw = now_s(); s_dirty = true; }
        }
        if (s_dirty && !down) {
            s_dirty = false;
            draw(c, buf);
        }
    }
}
