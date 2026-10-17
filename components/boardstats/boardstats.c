#include "boardstats.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "boardstats_secrets.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_private/esp_clk.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "boardstats";

typedef struct { const char *ssid, *pass; } net_t;
static const net_t NETS[] = BS_NETS;
#define NNETS ((int)(sizeof NETS / sizeof NETS[0]))

#define GOT_IP  BIT0
#define DROPPED BIT1

static EventGroupHandle_t s_ev;
static int s_net;                         /* the network that worked last */
static temperature_sensor_handle_t s_tsens;
static bool s_tsens_tried;
static int s_join_ms = -1, s_post_ms = -1;     /* this report's join, the last one's POST */
static unsigned s_sent, s_failed;

/* ---- the report, as JSON --------------------------------------------------- */

static char s_buf[3072];
static size_t s_len;
static bool s_first;                      /* nothing in this section yet */

static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...)
{
    if (s_len >= sizeof s_buf) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s_buf + s_len, sizeof s_buf - s_len, fmt, ap);
    va_end(ap);
    s_len = n < 0 ? sizeof s_buf : s_len + (size_t)n;
}

static void put_str(const char *s)
{
    put("\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') put("\\%c", c);
        else if (c < 0x20) put("\\u%04x", c);
        else put("%c", c);
    }
    put("\"");
}

static void key(const char *k)
{
    put(s_first ? "" : ",");
    s_first = false;
    put_str(k);
    put(":");
}

void boardstats_num(const char *k, double v)
{
    if (isnan(v) || isinf(v)) return;
    key(k);
    put("%.6g", v);
}

void boardstats_str(const char *k, const char *v)
{
    if (!v) return;
    key(k);
    put_str(v);
}

static const char *reset_reason(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_SW:        return "restart";
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_USB:       return "usb";
    default:                return "other";
    }
}

static void sys_section(void)
{
    boardstats_num("uptime_s", (double)(esp_timer_get_time() / 1000000));
    boardstats_num("heap_kb", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
    boardstats_num("heap_min_kb", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
    size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (ps) boardstats_num("psram_free_kb", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0);
    if (!s_tsens_tried) {       /* a board that has its own sensor keeps it, and reports chip_c itself */
        s_tsens_tried = true;
        temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
        if (temperature_sensor_install(&c, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK)
            s_tsens = NULL;
    }
    float t;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) boardstats_num("chip_c", t);
    boardstats_num("heap_big_kb", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024.0);
    boardstats_num("cpu_mhz", esp_clk_cpu_freq() / 1000000.0);
    boardstats_num("tasks", uxTaskGetNumberOfTasks());
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        boardstats_num("rssi_dbm", ap.rssi);
        boardstats_str("network", (const char *)ap.ssid);
        boardstats_num("wifi_ch", ap.primary);
        boardstats_num("wifi_mhz", ap.primary == 14 ? 2484 : 2407 + 5 * ap.primary);
        boardstats_str("wifi", ap.phy_11ax ? "802.11ax" : ap.phy_11n ? "802.11n" : ap.phy_11g ? "802.11g" : "802.11b");
    }
    int8_t tx;
    if (esp_wifi_get_max_tx_power(&tx) == ESP_OK) boardstats_num("tx_dbm", tx * 0.25);
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
        char a[16];
        snprintf(a, sizeof a, IPSTR, IP2STR(&ip.ip));
        boardstats_str("ip", a);
    }
    if (s_join_ms >= 0) boardstats_num("join_ms", s_join_ms);
    if (s_post_ms >= 0) boardstats_num("post_ms", s_post_ms);
    boardstats_num("sent", s_sent);
    boardstats_num("failed", s_failed);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    char chip[64];
    snprintf(chip, sizeof chip, "ESP32-S3 v%d.%d, %d cores, %lu MB flash, %u MB PSRAM",
             ci.revision / 100, ci.revision % 100, ci.cores, (unsigned long)(flash >> 20), (unsigned)(ps >> 20));
    boardstats_str("chip", chip);
    boardstats_str("idf", esp_get_idf_version());
    boardstats_str("last_reset", reset_reason());
}

static void build(const char *board, int every_s, boardstats_fill_t fill)
{
    uint8_t m[6] = {0};
    esp_efuse_mac_get_default(m);
    char mac[18];
    snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    const esp_app_desc_t *app = esp_app_get_description();
    char fw[48];
    snprintf(fw, sizeof fw, "%s %s", app->project_name, app->date);

    s_len = 0;
    put("{\"board\":");
    put_str(board);
    put(",\"mac\":\"%s\",\"every\":%d,\"fw\":", mac, every_s);
    put_str(fw);
    put(",\"sys\":{");
    s_first = true;
    sys_section();
    put("},\"stats\":{");
    s_first = true;
    if (fill) fill();
    put("}}");
}

/* ---- the network ----------------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupClearBits(s_ev, DROPPED);
        xEventGroupSetBits(s_ev, GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, GOT_IP);
        xEventGroupSetBits(s_ev, DROPPED);
    }
}

static bool setup(void)
{
    if (s_ev) return true;
    esp_netif_init();
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return false;
    wifi_mode_t mode;
    bool inited = esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT;
    if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        /* A board that runs ESP-NOW started WiFi with no network interface.
           One attached to a running radio never hears STA_START and its DHCP
           client fails at the first join (blinky1 crashed on it): stop the
           radio first, and join() starts it again. */
        if (inited) esp_wifi_stop();
        esp_netif_create_default_wifi_sta();
    }
    if (!inited) {
        wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
        if (esp_wifi_init(&c) != ESP_OK) return false;
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    s_ev = xEventGroupCreate();
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, NULL);
    return true;
}

static bool join(int timeout_ms)
{
    if (xEventGroupGetBits(s_ev) & GOT_IP) return true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (int i = 0; i < NNETS && esp_timer_get_time() < until; i++) {
        int k = (s_net + i) % NNETS;
        wifi_config_t c = {0};
        snprintf((char *)c.sta.ssid, sizeof c.sta.ssid, "%s", NETS[k].ssid);
        snprintf((char *)c.sta.password, sizeof c.sta.password, "%s", NETS[k].pass);
        c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        esp_wifi_disconnect();
        xEventGroupClearBits(s_ev, GOT_IP | DROPPED);
        esp_wifi_set_config(WIFI_IF_STA, &c);
        if (esp_wifi_connect() != ESP_OK) continue;
        int left = (int)((until - esp_timer_get_time()) / 1000);
        int wait = left < 12000 ? left : 12000;
        EventBits_t b = xEventGroupWaitBits(s_ev, GOT_IP | DROPPED, pdFALSE, pdFALSE, pdMS_TO_TICKS(wait > 0 ? wait : 1));
        if (b & GOT_IP) {
            s_net = k;
            return true;
        }
    }
    return false;
}

static void let_go(void)
{
    esp_wifi_disconnect();
    esp_wifi_stop();
    xEventGroupClearBits(s_ev, GOT_IP);
}

bool boardstats_report(const char *board, int every_s, boardstats_fill_t fill, bool keep, int timeout_ms)
{
    if (!BS_URL[0] || NNETS == 0) return false;
    if (!setup()) return false;
    int64_t t0 = esp_timer_get_time();
    bool was_up = xEventGroupGetBits(s_ev) & GOT_IP;
    if (!join(timeout_ms)) {
        ESP_LOGW(TAG, "no network");
        s_failed++;
        if (!keep) let_go();
        return false;
    }
    if (!was_up) s_join_ms = (int)((esp_timer_get_time() - t0) / 1000);
    build(board, every_s, fill);
    int64_t t1 = esp_timer_get_time();
    esp_http_client_config_t hc = {
        .url = BS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&hc);
    int status = -1;
    if (h) {
        esp_http_client_set_header(h, "Content-Type", "application/json");
        esp_http_client_set_header(h, "Authorization", "Bearer " BS_TOKEN);
        esp_http_client_set_post_field(h, s_buf, (int)s_len);
        if (esp_http_client_perform(h) == ESP_OK) status = esp_http_client_get_status_code(h);
        esp_http_client_cleanup(h);
    }
    s_post_ms = (int)((esp_timer_get_time() - t1) / 1000);
    if (status == 200) s_sent++;
    else s_failed++;
    if (!keep) let_go();
    if (status == 200) ESP_LOGI(TAG, "reported (%u bytes, %lld ms)", (unsigned)s_len, (esp_timer_get_time() - t0) / 1000);
    else ESP_LOGW(TAG, "report not taken: %d", status);
    return status == 200;
}

typedef struct { const char *board; int every_s; boardstats_fill_t fill; } job_t;

static void task(void *arg)
{
    job_t j = *(job_t *)arg;
    vPortFree(arg);
    vTaskDelay(pdMS_TO_TICKS(5000));              /* let the board finish coming up */
    for (;;) {
        boardstats_report(j.board, j.every_s, j.fill, true, 30000);
        vTaskDelay(pdMS_TO_TICKS(j.every_s * 1000));
    }
}

void boardstats_start(const char *board, int every_s, boardstats_fill_t fill)
{
    if (!BS_URL[0]) {
        ESP_LOGW(TAG, "no BOARDS_URL in secrets/boards.env at build time: not reporting");
        return;
    }
    job_t *j = pvPortMalloc(sizeof *j);
    if (!j) return;
    *j = (job_t){board, every_s, fill};
    xTaskCreate(task, "boardstats", 6144, j, 2, NULL);
}
