/*
 * spy: a desk time-lapse that reports over 4G.
 *
 * Weekdays from 07:00 to 16:59 it takes a 640x480 frame every minute onto the
 * card; at the top of each hour from 08:00 to 17:00 the hour before becomes a
 * six-second MP4 (10 fps) and goes to Telegram. Telegram messages to the bot
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
#include "clip.h"
#include "net.h"
#include "sched.h"
#include "store.h"
#include "tg.h"

static const char *TAG = "spy";

/* ---- messages between the tasks ----------------------------------------- */

typedef enum { CAM_PIC, CAM_PIC_FULL, CAM_STATUS, CAM_HELLO, CAM_TEST_FRAMES } cam_req_t;
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
static volatile bool s_encoding, s_cam_busy;

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
static bool battery(float *volts, float *pct)
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

static void status_text(char *out, size_t cap)
{
    struct tm lt;
    bool timed = local_now(&lt);
    char when[32] = "no clock yet";
    if (timed) strftime(when, sizeof when, "%a %H:%M", &lt);
    int64_t up = esp_timer_get_time() / 1000000;
    uint32_t fr = 0, tot = 0;
    bool card = store_space(&fr, &tot);
    float v = 0, pct = 0;
    bool bat = battery(&v, &pct);
    char batt[48];
    if (!bat) snprintf(batt, sizeof batt, "no reading");
    else if (v < 2.5f) snprintf(batt, sizeof batt, "none fitted (on USB)");
    else snprintf(batt, sizeof batt, "%.0f%% (%.2f V)", pct, v);
    int n = snprintf(out, cap,
        "spy, %s\n"
        "4G signal %d/31, up %lldh%02lldm\n"
        "card: %s%lu MB free of %lu\n"
        "today: %d frames, %d clips sent\n"
        "battery: %s\n"
        "chip %.0f C, RAM %u KB free%s%s",
        when, net_csq(), up / 3600, (up / 60) % 60,
        card ? "" : "MISSING ", (unsigned long)fr, (unsigned long)tot,
        s_frames_today, s_clips_today, batt, chip_temp(),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        cam_flip() ? ", picture flipped" : "", s_encoding ? ", encoding now" : "");
    if (timed && !sched_capture(&lt) && n > 0 && (size_t)n < cap)
        snprintf(out + n, cap - n, "\nframes: weekdays 07:00-17:00");
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
    if (cam_shot(FRAMESIZE_VGA, 12, path, &bytes)) s_frames_today++;
}

static void cam_task(void *arg)
{
    int last_min = -1;
    for (;;) {
        cam_req_t req;
        if (xQueueReceive(s_cam_q, &req, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_cam_busy = true;
            if (req == CAM_PIC || req == CAM_PIC_FULL) take_pic(req == CAM_PIC_FULL);
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
        int stamp = lt.tm_yday * 1440 + lt.tm_hour * 60 + lt.tm_min;
        if (stamp == last_min) continue;
        last_min = stamp;
        if (lt.tm_mday != s_today_mday) {
            s_today_mday = lt.tm_mday;
            s_frames_today = s_clips_today = 0;
        }
        if (store_ok() && sched_capture(&lt)) minute_frame(&lt);
        int h = sched_clip_hour(&lt);
        if (h >= 0) {
            work_req_t w = { .kind = WORK_CLIP, .hour = h, .last_min = 59 };
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
        char dir[32], out[64], cap[96];
        snprintf(dir, sizeof dir, "/sdcard/tl/%s", w.day);
        if (w.kind == WORK_CLIP) snprintf(out, sizeof out, "%s/%02d.mp4", dir, w.hour);
        else snprintf(out, sizeof out, "%s/%02d-sofar.mp4", dir, w.hour);
        int frames = 0;
        s_encoding = true;
        bool ok = store_ok() && clip_make(dir, w.hour, w.last_min, out, &frames);
        s_encoding = false;
        if (!ok) {
            if (w.kind == WORK_CLIP_SO_FAR) send_text("No frames yet this hour.");
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
            snprintf(cap, sizeof cap, "%02d:00-%02d:00, %s (%d frames)", w.hour, w.hour + 1, date, frames);
        else
            snprintf(cap, sizeof cap, "%02d:00-%02d:%02d so far, %s (%d frames)", w.hour, w.hour, w.last_min, date, frames);
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
        send_text("Making the clip of this hour so far...");
        xQueueSend(s_work_q, &r, 0);
    } else if (!strcmp(w, "status")) {
        cam_req_t r = CAM_STATUS;
        xQueueSend(s_cam_q, &r, 0);
    } else if (!strcmp(w, "flip")) {
        cam_set_flip(!cam_flip());
        settings_save_flip(cam_flip());
        send_text(cam_flip() ? "Picture flipped (upside down)." : "Picture the right way up.");
    } else {
        send_text("spy knows: pic (pic full for 5 MP), clip (this hour so far), status, flip.");
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

static bool do_job(net_job_t *j)
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
        if (!net_ok()) {
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
            ok = tg_poll(&offset, busy ? 2 : 20, on_tg_text);
        }
        if (ok) { fails = 0; last_ok = esp_timer_get_time(); }
        else if (++fails >= 3) {
            ESP_LOGW(TAG, "three failures in a row: redialling");
            net_up();
            fails = 0;
        }
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
    settings_load();
    store_mount();
    cam_sleep();                                /* the sensor powers up awake */
    ESP_LOGI(TAG, "telegram %s, card %s, internal RAM %u free", tg_configured() ? "configured" : "NOT CONFIGURED",
             store_ok() ? "in" : "MISSING", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    s_cam_q = xQueueCreate(4, sizeof(cam_req_t));
    s_work_q = xQueueCreate(16, sizeof(work_req_t));
    s_net_q = xQueueCreate(16, sizeof(net_job_t *));
    xTaskCreatePinnedToCore(cam_task, "cam", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(work_task, "work", 32768, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(net_task, "net", 12288, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(console_task, "console", 6144, NULL, 2, NULL, 0);
}
