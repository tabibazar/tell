#include "net.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_modem_api.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define MODEM_PWR  GPIO_NUM_21
#define MODEM_RX   17
#define MODEM_TX   18
#define APN        "mobile.bm"
#define FAST_BAUD  921600
#define FAST_BAUD_STR "921600"

static const char *TAG = "net";
static esp_modem_dce_t *s_dce;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_ev;
#define EV_IP   BIT0
static int s_csq = 99;
static char s_operator[40] = "no carrier yet";
static bool s_time;

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "PPP up: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_ev, EV_IP);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP lost its address");
        xEventGroupClearBits(s_ev, EV_IP);
    }
}

bool net_ok(void) { return s_ev && (xEventGroupGetBits(s_ev) & EV_IP); }
bool net_time_ok(void) { return s_time; }
int net_csq(void) { return s_csq; }
const char *net_operator(void) { return s_operator; }

/* "+COPS: 0,0,"Freedom Mobile",7": the carrier's long name. Asked while
   dialling, the only time the UART is free for AT. */
static void read_operator(void)
{
    char out[128] = "";
    esp_modem_at(s_dce, "AT+COPS=3,0", out, 2000);       /* names, not "302490" */
    out[0] = 0;
    if (esp_modem_at(s_dce, "AT+COPS?", out, 3000) != ESP_OK) return;
    const char *a = strchr(out, '"'), *b = a ? strchr(a + 1, '"') : NULL;
    if (a && b && b > a + 1) snprintf(s_operator, sizeof s_operator, "%.*s", (int)(b - a - 1), a + 1);
}

/* The modem's network time, "+CCLK: "26/10/02,01:58:25-16"". On this SIM it
   is UTC -- 01:58 when the Mac said 21:58 EDT -- with the zone after it, so
   the zone is ignored. Only used until SNTP answers, and only if plausible. */
static void time_from_modem(void)
{
    char out[128] = "";
    if (esp_modem_at(s_dce, "AT+CCLK?", out, 3000) != ESP_OK) return;
    int yy, mo, dd, hh, mi, ss;
    const char *q = strchr(out, '"');
    if (!q || sscanf(q + 1, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss) != 6) return;
    if (yy < 26 || yy > 60) return;            /* the modem's 1970/2004 before NITZ */
    struct tm t = { .tm_year = yy + 100, .tm_mon = mo - 1, .tm_mday = dd,
                    .tm_hour = hh, .tm_min = mi, .tm_sec = ss };
    setenv("TZ", "UTC0", 1); tzset();
    time_t utc = mktime(&t);
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); tzset();
    struct timeval tv = { .tv_sec = utc };
    settimeofday(&tv, NULL);
    s_time = true;
    ESP_LOGI(TAG, "clock from the modem: %s", out);
}

static void modem_power_on(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << MODEM_PWR, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    if (gpio_get_level(MODEM_PWR) == 0) {
        gpio_set_level(MODEM_PWR, 1);
        /* UART is ready 55 ms after power, but registering takes seconds. */
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

bool net_up(void)
{
    static bool once;
    if (!once) {
        once = true;
        s_ev = xEventGroupCreate();
        esp_netif_init();
        esp_event_loop_create_default();
        esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip, NULL);
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
        tzset();
    }
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        esp_modem_destroy(s_dce);
        s_dce = NULL;
    }
    if (s_netif) {
        esp_netif_destroy(s_netif);
        s_netif = NULL;
    }
    xEventGroupClearBits(s_ev, EV_IP);
    modem_power_on();

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_PPP();
    s_netif = esp_netif_new(&ncfg);
    esp_modem_dte_config_t dte = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte.uart_config.tx_io_num = MODEM_TX;
    dte.uart_config.rx_io_num = MODEM_RX;
    dte.uart_config.rts_io_num = -1;
    dte.uart_config.cts_io_num = -1;
    dte.uart_config.rx_buffer_size = 16 * 1024;
    dte.uart_config.tx_buffer_size = 4 * 1024;
    dte.uart_config.event_queue_size = 40;
    dte.dte_buffer_size = 4 * 1024;
    dte.task_stack_size = 6144;
    dte.task_priority = 8;
    esp_modem_dce_config_t dce = ESP_MODEM_DCE_DEFAULT_CONFIG(APN);
    s_dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte, &dce, s_netif);
    if (!s_dce) { ESP_LOGE(TAG, "no modem device"); return false; }

    /* The modem may still be at the fast rate from before a reset of ours
       (it keeps power through one when the "4G" DIP is on), and still in a
       PPP call: then it answers nothing to AT until it sees "+++" with a
       second of silence either side. esp_modem sends that escape only once
       and at whatever rate it is at, so try both rates, then the escape at
       both rates, and again. */
    bool synced = false;
    int rate = 115200;
    for (int round = 0; round < 4 && !synced; round++) {
        static const int rates[2] = { 115200, FAST_BAUD };
        for (int k = 0; k < 2 && !synced; k++) {
            rate = rates[k];
            uart_set_baudrate(UART_NUM_1, rate);
            synced = esp_modem_sync(s_dce) == ESP_OK || esp_modem_sync(s_dce) == ESP_OK;
        }
        for (int k = 0; k < 2 && !synced; k++) {
            rate = rates[k];
            uart_set_baudrate(UART_NUM_1, rate);
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_write_bytes(UART_NUM_1, "+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            synced = esp_modem_sync(s_dce) == ESP_OK || esp_modem_sync(s_dce) == ESP_OK;
            if (synced) ESP_LOGI(TAG, "modem was still in a call at %d baud", rate);
        }
    }
    if (!synced) { ESP_LOGE(TAG, "modem does not answer AT"); return false; }
    /* 115200 moves ~10 KB/s, so a 270 KB photo took 40 s. AT+IPR (not
       IPREX) is the A76xx family's temporary rate: it is forgotten when the
       modem loses power, so nothing here can strand a later boot. */
    if (rate != FAST_BAUD) {
        char out[32] = "";
        if (esp_modem_at(s_dce, "AT+IPR=" FAST_BAUD_STR, out, 1000) == ESP_OK) {
            uart_set_baudrate(UART_NUM_1, FAST_BAUD);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (esp_modem_sync(s_dce) == ESP_OK && esp_modem_sync(s_dce) == ESP_OK) {
                rate = FAST_BAUD;
            } else {
                ESP_LOGW(TAG, "no answer at %d; back to 115200", FAST_BAUD);
                uart_set_baudrate(UART_NUM_1, 115200);
                esp_modem_at(s_dce, "AT+IPR=115200", out, 1000);
            }
        }
    }
    ESP_LOGI(TAG, "modem at %d baud", rate);
    char echo[32];
    esp_modem_at(s_dce, "ATE0", echo, 1000);
    esp_modem_at(s_dce, "ATH", echo, 3000);     /* drop a call left over from before */

    /* Registered? Wait for it rather than dial into nothing. */
    for (int i = 0; i < 60; i++) {
        int rssi = 99, ber = 99;
        char out[96] = "";
        esp_modem_get_signal_quality(s_dce, &rssi, &ber);
        s_csq = rssi;
        esp_modem_at(s_dce, "AT+CEREG?", out, 2000);
        if (strstr(out, ",1") || strstr(out, ",5")) {
            ESP_LOGI(TAG, "registered, CSQ %d", rssi);
            break;
        }
        if (i % 5 == 0) ESP_LOGI(TAG, "waiting to register (CSQ %d, %s)", rssi, out);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Only before SNTP: a redial must not step a good clock to the modem's
       whole seconds -- a step back over 08:00 would send hour 7 twice. */
    if (!s_time) time_from_modem();
    read_operator();

    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "could not enter data mode");
        return false;
    }
    if (!(xEventGroupWaitBits(s_ev, EV_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(60000)) & EV_IP)) {
        ESP_LOGE(TAG, "no PPP address in 60 s");
        return false;
    }

    static bool sntp_started;
    if (!sntp_started) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
            ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
        esp_netif_sntp_init(&sc);
        sntp_started = true;
    }
    /* After the first answer SNTP keeps the clock itself (hourly); waiting
       for another answer on a redial would only wait. */
    static bool sntp_synced;
    if (!sntp_synced && esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)) == ESP_OK) {
        sntp_synced = s_time = true;
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        char b[32];
        strftime(b, sizeof b, "%a %F %T %Z", &lt);
        ESP_LOGI(TAG, "SNTP: %s", b);
    } else if (!sntp_synced) {
        ESP_LOGW(TAG, "SNTP did not answer; %s", s_time ? "keeping the modem's time" : "no time yet");
    }
    return true;
}
