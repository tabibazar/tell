/*
 * spy: a desk time-lapse that reports over 4G.
 *
 * Weekdays from 07:00 to 16:59 it takes a 640x480 frame every minute onto the
 * card; at 17:00 the whole day becomes one MP4 (10 fps, about a minute) and
 * goes to Telegram. Telegram messages to the bot
 * work any time: "pic" sends a photo, "clip" the hour so far, "status" how
 * spy is, "flip" turns the picture upside down, "help" lists them.
 *
 * Three tasks, each the only one touching its hardware:
 *   cam   the OV5640 and the I2C bus (fuel gauge): the minute frames, pics
 *   work  the encoder: clips, and clearing old days off the card
 *   net   the modem and Telegram: sending, and polling for commands
 * They talk by queues. The console (UART0, the CH343) takes the same
 * commands as Telegram, plus "dump <path>" to pull a file to the Mac.
 */
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "cam.h"
#include "camlink_tx.h"
#include "clip.h"
#include "net.h"
#include "sched.h"
#include "store.h"
#include "tg.h"
#include "where.h"

static const char *TAG = "spy";

/* ---- messages between the tasks ----------------------------------------- */

typedef enum { CAM_PIC, CAM_PIC_FULL, CAM_STATUS, CAM_HELLO, CAM_TEST_FRAMES, CAM_FORMAT, CAM_BATTEST, CAM_TINY } cam_req_t;
typedef enum { WORK_CLIP, WORK_CLIP_SO_FAR } work_kind_t;
typedef struct { work_kind_t kind; char day[9]; int hour, last_min; } work_req_t;
typedef enum { NET_TEXT, NET_PHOTO, NET_DOC, NET_VIDEO } net_kind_t;
typedef struct {
    net_kind_t kind;
    int tries;
    char path[64];
    char text[400];             /* the message, or the caption */
} net_job_t;

static QueueHandle_t s_cam_q, s_work_q, s_net_q;
static int s_frames_today, s_clips_today, s_today_mday = -1;
static volatile bool s_encoding, s_cam_busy, s_redial, s_uploading, s_cell_req, s_where_req;

static void send_text(const char *text)
{
    net_job_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM);
    if (!j) return;
    j->kind = NET_TEXT;
    snprintf(j->text, sizeof j->text, "%s", text);
    if (xQueueSend(s_net_q, &j, 0) != pdTRUE) free(j);
}

static void send_file(net_kind_t kind, const char *path, const char *caption)
{
    net_job_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM);
    if (!j) return;
    j->kind = kind;
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->text, sizeof j->text, "%s", caption);
    if (xQueueSend(s_net_q, &j, 0) != pdTRUE) free(j);
}

static bool local_now(struct tm *lt)
{
    time_t now = time(NULL);
    localtime_r(&now, lt);
    return net_time_ok() && lt->tm_year > 125;
}

/* ---- settings ------------------------------------------------------------ */

static void settings_load(void)
{
    nvs_handle_t h;
    uint8_t flip = 0;
    if (nvs_open("spy", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "flip", &flip);
        nvs_close(h);
    }
    cam_set_flip(flip);
}

static void settings_save_flip(bool flip)
{
    nvs_handle_t h;
    if (nvs_open("spy", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "flip", flip);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- status -------------------------------------------------------------- */

/* MAX17048 on the camera's I2C bus (GPIO15/16), so only from the cam task,
   between shots. Its pull-ups are on the camera rail: with the CAM DIP off
   there is no reading. */
static bool battery(float *volts, float *pct, float *rate)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = 0, .sda_io_num = 15, .scl_io_num = 16,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x36, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev;
    bool ok = false;
    if (i2c_master_bus_add_device(bus, &dc, &dev) == ESP_OK) {
        uint8_t reg = 0x02, v[2], s[2];
        if (i2c_master_transmit_receive(dev, &reg, 1, v, 2, 100) == ESP_OK) {
            reg = 0x04;
            if (i2c_master_transmit_receive(dev, &reg, 1, s, 2, 100) == ESP_OK) {
                *volts = ((v[0] << 8) | v[1]) * 78.125e-6f;
                *pct = s[0] + s[1] / 256.0f;
                uint8_t c[2];
                reg = 0x16;                         /* CRATE: 0.208 %/h a bit, signed */
                *rate = i2c_master_transmit_receive(dev, &reg, 1, c, 2, 100) == ESP_OK
                        ? (int16_t)((c[0] << 8) | c[1]) * 0.208f : 0;
                if (*pct > 100) *pct = 100;      /* uncalibrated gauges overshoot */
                ok = true;
            }
        }
        i2c_master_bus_rm_device(dev);
    }
    i2c_del_master_bus(bus);
    return ok;
}

static float chip_temp(void)
{
    static temperature_sensor_handle_t ts;
    if (!ts) {
        temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&c, &ts) != ESP_OK) return -99;
        temperature_sensor_enable(ts);
    }
    float t = -99;
    temperature_sensor_get_celsius(ts, &t);
    return t;
}

/* LTE band to its frequency, for the bands Canadian carriers use. */
static const char *band_mhz(int band)
{
    switch (band) {
    case 2: case 25: return "1900 MHz";
    case 4: case 66: return "AWS 1700/2100 MHz";
    case 5: case 26: return "850 MHz";
    case 7:  return "2600 MHz";
    case 12: case 13: case 14: case 17: return "700 MHz";
    case 30: return "2300 MHz";
    case 41: return "2500 MHz";
    case 71: return "600 MHz";
    default: return "";
    }
}

static const char *rsrp_word(int rsrp)
{
    return rsrp >= -80 ? "excellent" : rsrp >= -90 ? "good" : rsrp >= -100 ? "fair" : rsrp >= -110 ? "weak" : "very weak";
}

static void cell_text(const net_cell_t *c, char *out, size_t cap)
{
    if (!c->ok) { snprintf(out, cap, "no LTE cell reading"); return; }
    snprintf(out, cap,
             "%s (%d-%03d), LTE band %d %s\n"
             "tower (eNB) %ld, sector %ld, PCI %d, area 0x%X\n"
             "signal %d dBm (%s), quality %d dB, SINR %d dB\n"
             "cellmapper.net: network %d-%d, search eNB %ld",
             net_operator(), c->mcc, c->mnc, c->band, band_mhz(c->band),
             c->cell >> 8, c->cell & 0xFF, c->pci, c->tac,
             c->rsrp, rsrp_word(c->rsrp), c->rsrq, c->sinr,
             c->mcc, c->mnc, c->cell >> 8);
}

static void status_text(char *out, size_t cap)
{
    struct tm lt;
    bool timed = local_now(&lt);
    char when[32] = "no clock yet";
    if (timed) strftime(when, sizeof when, "%a %H:%M", &lt);
    int64_t up = esp_timer_get_time() / 1000000;
    char cellline[64] = "";
    net_cell_t cl = net_cell_last();
    if (cl.ok) snprintf(cellline, sizeof cellline, "tower %ld band %d, %d dBm\n", cl.cell >> 8, cl.band, cl.rsrp);
    uint32_t fr = 0, tot = 0;
    bool card = store_space(&fr, &tot);
    float v = 0, pct = 0, rate = 0;
    bool bat = battery(&v, &pct, &rate);
    char batt[80];
    /* The rate tells a cell from none: with no cell (or the holder's switch
       off) the gauge sees the charger's ~4.2 V and nothing moves. */
    if (!bat) snprintf(batt, sizeof batt, "no reading");
    else if (v < 2.5f) snprintf(batt, sizeof batt, "none fitted (on USB)");
    else snprintf(batt, sizeof batt, "%.0f%% (%.2f V), %s %.1f%%/h", pct, v,
                  rate > 0.5f ? "charging" : rate < -0.5f ? "draining" : "steady", rate < 0 ? -rate : rate);
    int n = snprintf(out, cap,
        "spy, %s\n"
        "4G: %s, signal %d/31, up %lldh%02lldm\n"
        "%s"
        "card: %s%lu MB free of %lu\n"
        "today: %d frames, %d clips sent\n"
        "battery: %s\n"
        "chip %.0f C, RAM %u KB free%s%s",
        when, net_operator(), net_csq(), up / 3600, (up / 60) % 60, cellline,
        card ? "" : "MISSING ", (unsigned long)fr, (unsigned long)tot,
        s_frames_today, s_clips_today, batt, chip_temp(),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        cam_flip() ? ", picture flipped" : "", s_encoding ? ", encoding now" : "");
    if (timed && !sched_capture(&lt) && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - n, "\nframes: weekdays 07:00-17:00");
}

/* ---- battery watch ------------------------------------------------------- */

/* The last gauge reading, for captions: -1 until there is one, or with no
   cell fitted. Written by the cam task only (the gauge is on its bus). */
static volatile int s_batt_pct = -1;

/* Once a minute from the cam task: a Telegram warning at 20, 10 and 5 %,
   each once a discharge, with the hours left at the present drain. Charged
   back past 30 % and they are armed again. (2026-10-02 spy ran flat at
   15:51 with no word; Reza: "add the battery warning to telegram".) */
static void battery_watch(bool test)
{
    static const int LEVELS[] = { 20, 10, 5 };
    static int warned = 100;                    /* the lowest level warned at */
    float v, pct, rate;
    if (!battery(&v, &pct, &rate) || v < 2.5f) {   /* none fitted */
        s_batt_pct = -1;
        if (test) send_text("TEST battery warning: no battery reading (none fitted, or the CAM DIP is off).");
        return;
    }
    int p = (int)(pct + 0.5f);
    s_batt_pct = p;
    if (test) {                                 /* console 'battest': the real message, marked */
        char t[200];
        int n = snprintf(t, sizeof t, "TEST of the battery warning. spy battery %d%% (%.2f V)", p, (double)v);
        if (rate < -0.5f) snprintf(t + n, sizeof t - n, ", about %.0f h left at this rate.", (double)(pct / -rate));
        else snprintf(t + n, sizeof t - n, ", charging or full.");
        send_text(t);
        return;
    }
    if (p >= 30) { warned = 100; return; }
    for (size_t i = 0; i < sizeof LEVELS / sizeof LEVELS[0]; i++) {
        int L = LEVELS[i];
        if (p > L || warned <= L) continue;
        warned = L;
        char t[160];
        int n = snprintf(t, sizeof t, "spy battery %d%% (%.2f V)", p, (double)v);
        if (rate < -0.5f)
            snprintf(t + n, sizeof t - n, ", about %.0f h left at this rate.", (double)(pct / -rate));
        else
            snprintf(t + n, sizeof t - n, ".");
        if (L == 5) strlcat(t, " Plug it in or it stops soon.", sizeof t);
        ESP_LOGW(TAG, "%s", t);
        send_text(t);
        break;
    }
}

/* ---- cam task ------------------------------------------------------------ */

static void take_pic(bool full)
{
    struct tm lt;
    char path[64], day[9];
    if (!store_ok()) { send_text("No card in spy, so no pictures."); return; }
    if (local_now(&lt)) {
        sched_day(&lt, day);
        snprintf(path, sizeof path, "/sdcard/pics/%s-%02d%02d%02d.jpg", day, lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        snprintf(path, sizeof path, "/sdcard/pics/boot-%lld.jpg", esp_timer_get_time() / 1000000);
    }
    size_t bytes;
    /* QXGA (3 MP, ~230 KB, ~25 s over 4G) for a quick look; the full 5 MP
       sensor as a document (uncompressed by Telegram, ~460 KB) on request. */
    if (!cam_shot(full ? FRAMESIZE_QSXGA : FRAMESIZE_QXGA, 10, path, &bytes)) {
        send_text("The camera did not give a picture.");
        return;
    }
    char cap[64];
    if (net_time_ok()) strftime(cap, sizeof cap, "%a %H:%M:%S", &lt);
    else snprintf(cap, sizeof cap, "spy");
    send_file(full ? NET_DOC : NET_PHOTO, path, cap);
}

static void minute_frame(const struct tm *lt)
{
    char day[9], dir[32], path[64];
    sched_day(lt, day);
    snprintf(dir, sizeof dir, "/sdcard/tl/%s", day);
    if (!store_mkdir(dir)) return;
    snprintf(path, sizeof path, "%s/%02d%02d.jpg", dir, lt->tm_hour, lt->tm_min);
    size_t bytes;
    if (cam_shot(FRAMESIZE_VGA, 12, path, &bytes)) {
        s_frames_today++;
        camlink_send_file(path, lt->tm_hour, lt->tm_min, lt->tm_wday);   /* tiny1's screen */
    }
}

/* tiny1 asked (its BOOT button): a fresh frame, sent to it alone. */
static void tiny_pic(void)
{
    if (!store_ok()) return;
    struct tm lt;
    bool timed = local_now(&lt);
    size_t bytes;
    if (cam_shot(FRAMESIZE_VGA, 12, "/sdcard/pics/tiny1.jpg", &bytes))
        camlink_send_file("/sdcard/pics/tiny1.jpg", timed ? lt.tm_hour : 0, timed ? lt.tm_min : 0,
                          timed ? lt.tm_wday : 0xFF);
}

static void on_tiny_ask(void)          /* WiFi task: hand it to the camera's owner */
{
    cam_req_t r = CAM_TINY;
    if (s_cam_q) xQueueSend(s_cam_q, &r, 0);
}

static void cam_task(void *arg)
{
    long last_min = -1;
    for (;;) {
        cam_req_t req;
        if (xQueueReceive(s_cam_q, &req, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_cam_busy = true;
            if (req == CAM_PIC || req == CAM_PIC_FULL) take_pic(req == CAM_PIC_FULL);
            else if (req == CAM_BATTEST) battery_watch(true);
            else if (req == CAM_TINY) tiny_pic();
            else if (req == CAM_FORMAT) {
                /* The cam task is the one that writes frames, so nothing of
                   its own is open; the encoder and an upload are waited out. */
                for (int i = 0; i < 300 && (s_encoding || s_uploading); i++) vTaskDelay(pdMS_TO_TICKS(1000));
                printf("FORMAT %s\n", !s_encoding && !s_uploading && store_format() ? "DONE" : "FAILED");
                fflush(stdout);
            }
            else if (req == CAM_TEST_FRAMES) {
                /* Console only: ten frames now, filed as minutes 00-09 of
                   this hour, to try "clip" outside the working hours. */
                struct tm lt;
                if (store_ok() && local_now(&lt))
                    for (int m = 0; m < 10; m++) { lt.tm_min = m; minute_frame(&lt); }
            }
            else {                              /* the gauge is on the camera's bus */
                char *t = malloc(600);
                if (t) {
                    int n = req == CAM_HELLO ? snprintf(t, 600, "spy is on.\n") : 0;
                    status_text(t + n, 600 - n);
                    ESP_LOGI(TAG, "%s", t);
                    send_text(t);
                    free(t);
                }
            }
            s_cam_busy = false;
        }
        struct tm lt;
        if (!local_now(&lt)) continue;
        /* Minutes since 1970, acted on only going forward: a clock stepped
           back across a minute must not take a frame or send a clip twice. */
        long stamp = (long)(time(NULL) / 60);
        if (stamp <= last_min) continue;
        last_min = stamp;
        if (lt.tm_mday != s_today_mday) {
            s_today_mday = lt.tm_mday;
            s_frames_today = s_clips_today = 0;
        }
        if (store_ok() && sched_capture(&lt)) minute_frame(&lt);
        battery_watch(false);                   /* after the frame: the gauge shares the camera's bus */
        if (sched_day_clip(&lt)) {                    /* 17:00: the whole day, one clip */
            work_req_t w = { .kind = WORK_CLIP, .hour = SCHED_LAST_HOUR, .last_min = 59 };
            sched_day(&lt, w.day);
            xQueueSend(s_work_q, &w, 0);
        }
    }
}

/* ---- work task ----------------------------------------------------------- */

/* Oldest day folder off the card while it is short of room. */
static void make_room(void)
{
    uint32_t fr, tot;
    while (store_space(&fr, &tot) && fr < 30) {
        DIR *d = opendir("/sdcard/tl");
        if (!d) return;
        char oldest[16] = "";
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strlen(e->d_name) == 8 && (!oldest[0] || strcmp(e->d_name, oldest) < 0))
                snprintf(oldest, sizeof oldest, "%s", e->d_name);
        }
        closedir(d);
        if (!oldest[0]) return;
        char dir[32], path[300];
        snprintf(dir, sizeof dir, "/sdcard/tl/%s", oldest);
        d = opendir(dir);
        while (d && (e = readdir(d))) {
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            remove(path);
        }
        if (d) closedir(d);
        rmdir(dir);
        ESP_LOGW(TAG, "card short of room: removed %s", dir);
    }
}

static void work_task(void *arg)
{
    for (;;) {
        work_req_t w;
        if (xQueueReceive(s_work_q, &w, portMAX_DELAY) != pdTRUE) continue;
        char dir[32], out[64], cap[128];
        snprintf(dir, sizeof dir, "/sdcard/tl/%s", w.day);
        if (w.kind == WORK_CLIP) snprintf(out, sizeof out, "%s/day.mp4", dir);
        else snprintf(out, sizeof out, "%s/sofar.mp4", dir);
        int frames = 0;
        s_encoding = true;
        bool ok = store_ok() && clip_make(dir, SCHED_FIRST_HOUR, w.hour, w.last_min, out, &frames);
        s_encoding = false;
        if (!ok) {
            if (w.kind == WORK_CLIP_SO_FAR) send_text("No frames yet today.");
            else ESP_LOGW(TAG, "no clip for %s %02d:00", w.day, w.hour);
            continue;
        }
        struct tm d = { 0 };
        d.tm_year = atoi(w.day) / 10000 - 1900;
        d.tm_mon = atoi(w.day) / 100 % 100 - 1;
        d.tm_mday = atoi(w.day) % 100;
        d.tm_isdst = -1;
        mktime(&d);
        char date[24];
        strftime(date, sizeof date, "%a %e %b", &d);
        if (w.kind == WORK_CLIP)
            snprintf(cap, sizeof cap, "%s, %02d:00-%02d:00 (%d frames)", date, SCHED_FIRST_HOUR, w.hour + 1, frames);
        else
            snprintf(cap, sizeof cap, "%s so far, %02d:00-%02d:%02d (%d frames)", date, SCHED_FIRST_HOUR,
                     w.hour, w.last_min, frames);
        if (s_batt_pct >= 0) {                  /* after either: the caption is always set first */
            size_t n = strlen(cap);
            snprintf(cap + n, sizeof cap - n, ", battery %d%%", s_batt_pct);
        }
        send_file(NET_VIDEO, out, cap);
        make_room();
    }
}

/* ---- commands (Telegram and console) ------------------------------------- */

static void command(const char *text)
{
    while (*text == ' ' || *text == '/') text++;
    char w[24] = "";
    sscanf(text, "%23s", w);
    for (char *p = w; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    bool full = strstr(text, "full") || strstr(text, "Full");
    ESP_LOGI(TAG, "command: %s", text);
    if (!strcmp(w, "pic") || !strcmp(w, "photo") || !strcmp(w, "picture")) {
        cam_req_t r = full ? CAM_PIC_FULL : CAM_PIC;
        xQueueSend(s_cam_q, &r, 0);
    } else if (!strcmp(w, "clip") || !strcmp(w, "video")) {
        struct tm lt;
        if (!local_now(&lt)) { send_text("No clock yet, so no clip."); return; }
        work_req_t r = { .kind = WORK_CLIP_SO_FAR, .hour = lt.tm_hour, .last_min = lt.tm_min };
        sched_day(&lt, r.day);
        send_text("Making today's clip so far; a minute of encoding for every fifty frames...");
        xQueueSend(s_work_q, &r, 0);
    } else if (!strcmp(w, "status")) {
        cam_req_t r = CAM_STATUS;
        xQueueSend(s_cam_q, &r, 0);
    } else if (!strcmp(w, "cell") || !strcmp(w, "tower") || !strcmp(w, "towers")) {
        send_text("Asking the modem; spy is offline for a few seconds.");
        s_cell_req = true;
    } else if (!strcmp(w, "where") || !strcmp(w, "location") || !strcmp(w, "gps")) {
        send_text("Finding spy from its cell tower; offline for a few seconds.");
        s_where_req = true;
    } else if (!strcmp(w, "flip")) {
        cam_set_flip(!cam_flip());
        settings_save_flip(cam_flip());
        send_text(cam_flip() ? "Picture flipped (upside down)." : "Picture the right way up.");
    } else {
        send_text("spy knows: pic (pic full for 5 MP), clip (today so far), status, cell, where, flip.");
    }
}

static void on_tg_text(const char *text, int64_t date)
{
    /* Old messages -- sent while spy was off or rebooting -- are not
       obeyed: a "pic" from an hour ago is not a request for one now. */
    if (date && time(NULL) - date > 120) {
        ESP_LOGI(TAG, "skipping an old message: %s", text);
        return;
    }
    command(text);
}

/* ---- net task ------------------------------------------------------------ */

static bool do_job_inner(net_job_t *j);

static bool do_job(net_job_t *j)
{
    s_uploading = j->kind != NET_TEXT;
    bool ok = do_job_inner(j);
    s_uploading = false;
    return ok;
}

static bool do_job_inner(net_job_t *j)
{
    switch (j->kind) {
    case NET_TEXT:  return tg_send_text(j->text);
    case NET_PHOTO: return tg_send_file("sendPhoto", "photo", j->path, "image/jpeg", j->text, NULL);
    case NET_DOC:   return tg_send_file("sendDocument", "document", j->path, "image/jpeg", j->text, NULL);
    case NET_VIDEO: {
        char extra[64];
        snprintf(extra, sizeof extra, "width=%d\nheight=%d\n", CLIP_W, CLIP_H);
        return tg_send_file("sendVideo", "video", j->path, "video/mp4", j->text, extra);
    }
    }
    return false;
}

static void net_task(void *arg)
{
    int fails = 0;
    int64_t offset = -1, last_ok = esp_timer_get_time();
    bool announced = false;
    for (;;) {
        if (!net_ok() || s_redial) {
            s_redial = false;
            tg_reset();
            if (!net_up()) {
                ESP_LOGW(TAG, "no network; trying again in 30 s");
                /* Half an hour without a link: start over from scratch. */
                if (esp_timer_get_time() - last_ok > 30LL * 60 * 1000000) esp_restart();
                vTaskDelay(pdMS_TO_TICKS(30000));
                continue;
            }
            fails = 0;
        }
        if (!announced) {
            announced = true;
            cam_req_t r = CAM_HELLO;
            xQueueSend(s_cam_q, &r, 0);
        }
        if (s_cell_req) {
            /* What is waiting goes first: the reading takes PPP down. */
            net_job_t *w = NULL;
            while (xQueueReceive(s_net_q, &w, 0) == pdTRUE) { do_job(w); free(w); }
            s_cell_req = false;
            net_cell_t c;
            net_cell_now(&c);
            tg_reset();                         /* the old connection died with PPP */
            if (!net_ok()) s_redial = true;
            char t[320];
            int n = snprintf(t, sizeof t, "spy's cell tower now:\n");
            cell_text(&c, t + n, sizeof t - n);
            send_text(t);
            continue;
        }
        if (s_where_req) {
            net_job_t *w = NULL;
            while (xQueueReceive(s_net_q, &w, 0) == pdTRUE) { do_job(w); free(w); }
            s_where_req = false;
            net_cell_t c;
            net_cell_now(&c);
            tg_reset();
            if (!net_ok()) { s_redial = true; send_text("No network after reading the cell."); continue; }
            double lat, lon;
            int acc;
            char t[200];
            if (where_lookup(&c, &lat, &lon, &acc)) {
                snprintf(t, sizeof t, "spy is near %.5f, %.5f (within about %d m, from tower %ld).",
                         lat, lon, acc, c.cell >> 8);
                tg_send_text(t);
                tg_send_location(lat, lon);
            } else {
                snprintf(t, sizeof t, "Could not place tower %ld (%d-%d, area 0x%X) on a map.",
                         c.cell >> 8, c.mcc, c.mnc, c.tac);
                tg_send_text(t);
            }
            continue;
        }
        net_job_t *j = NULL;
        bool ok;
        if (xQueueReceive(s_net_q, &j, 0) == pdTRUE) {
            ok = do_job(j);
            if (!ok && ++j->tries < 3) {
                if (xQueueSend(s_net_q, &j, 0) != pdTRUE) free(j);
            } else {
                if (ok && j->kind == NET_VIDEO) s_clips_today++;
                free(j);
            }
        } else {
            /* Short polls while something is being made to send. */
            bool busy = s_encoding || s_cam_busy || uxQueueMessagesWaiting(s_cam_q) > 0;
            ok = tg_poll(&offset, busy ? 2 : 45, on_tg_text);
        }
        /* Only failures to reach Telegram count toward a redial: an HTTP
           error from it (a 409, a refused file) means the link is fine. */
        if (ok || tg_reached()) { fails = 0; last_ok = esp_timer_get_time(); }
        else if (++fails >= 3) {
            ESP_LOGW(TAG, "three failures in a row: redialling");
            s_redial = true;
            fails = 0;
        }
        if (!ok && tg_reached()) vTaskDelay(pdMS_TO_TICKS(2000));   /* no tight loop on an HTTP error */
    }
}

/* ---- console ------------------------------------------------------------- */

static void dump_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("DUMPFAIL %s\n", path); return; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    printf("DUMPBEGIN %ld %s\n", len, path);
    unsigned char in[768], out[1100];
    size_t n, ol;
    while ((n = fread(in, 1, sizeof in, f)) > 0) {
        mbedtls_base64_encode(out, sizeof out, &ol, in, n);
        out[ol] = 0;
        printf("%s\n", out);
    }
    printf("DUMPEND\n");
    fflush(stdout);
    fclose(f);
}

static void console_task(void *arg)
{
    uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0);
    char line[128];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[n] = 0;
            if (n > 5 && !strncmp(line, "dump ", 5)) dump_file(line + 5);
            else if (n > 3 && !strncmp(line, "ls ", 3)) {
                DIR *d = opendir(line + 3);
                struct dirent *e;
                while (d && (e = readdir(d))) printf("LS %s\n", e->d_name);
                if (d) closedir(d);
                printf("LSEND\n");
            } else if (!strcmp(line, "battest")) {
                cam_req_t r = CAM_BATTEST;
                xQueueSend(s_cam_q, &r, 0);
            } else if (!strcmp(line, "format yes")) {
                cam_req_t r = CAM_FORMAT;
                xQueueSend(s_cam_q, &r, 0);
            } else if (!strcmp(line, "redial")) {
                s_redial = true;
            } else if (!strcmp(line, "testframes")) {
                cam_req_t r = CAM_TEST_FRAMES;
                xQueueSend(s_cam_q, &r, 0);
            } else if (n) command(line);
            n = 0;
        } else if (n < sizeof line - 1) {
            line[n++] = (char)c;
        }
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    /* One error line per dropped PPP packet, at 115200 baud, only slows
       spy further when PPP is busy; TCP retransmits them anyway.
       (This tag logs nothing else spy relies on: net.c logs the PPP state.) */
    esp_log_level_set("esp-netif_lwip-ppp", ESP_LOG_NONE);
    settings_load();
    store_mount();
    cam_sleep();                                /* the sensor powers up awake */
    ESP_LOGI(TAG, "telegram %s, card %s, internal RAM %u free", tg_configured() ? "configured" : "NOT CONFIGURED",
             store_ok() ? "in" : "MISSING", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    s_cam_q = xQueueCreate(4, sizeof(cam_req_t));
    if (!camlink_start(on_tiny_ask)) ESP_LOGW(TAG, "camlink (tiny1's photos) did not start");
    s_work_q = xQueueCreate(16, sizeof(work_req_t));
    s_net_q = xQueueCreate(16, sizeof(net_job_t *));
    xTaskCreatePinnedToCore(cam_task, "cam", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(work_task, "work", 32768, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(net_task, "net", 12288, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 2, NULL, 0);
}
