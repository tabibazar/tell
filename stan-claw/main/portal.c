#include "portal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mcpd.h"
#include "net.h"
#include "setup_web.h"

static const char *TAG = "portal";
static volatile bool s_on;
static void (*s_save)(const char *, const char *);
static bool s_registered;
#define PAGE_MAX 12288

/* ---- DNS: every name is this board ---- */

static void dns_task(void *arg)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct timeval tv = { .tv_sec = 1 };
    if (s < 0 || bind(s, (struct sockaddr *)&me, sizeof me) != 0) {
        ESP_LOGE(TAG, "DNS: no socket");
        if (s >= 0) close(s);
        vTaskDelete(NULL);
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    static uint8_t q[512];
    while (s_on) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int n = recvfrom(s, q, sizeof q - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* The question's end: its name (labels) then type and class. */
        int p = 12;
        while (p < n && q[p]) p += q[p] + 1;
        p += 5;
        if (p > n) continue;
        bool a = q[p - 4] == 0 && q[p - 3] == 1;            /* type A: answer it; anything else: no answers */
        q[2] = 0x81; q[3] = 0x80;                           /* a response, recursion available, no error */
        q[6] = 0; q[7] = a ? 1 : 0;                         /* answers */
        q[8] = q[9] = q[10] = q[11] = 0;
        int r = p;
        if (a) {
            static const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
            memcpy(q + r, ans, sizeof ans);
            r += sizeof ans;
        }
        sendto(s, q, r, 0, (struct sockaddr *)&from, fl);
    }
    close(s);
    vTaskDelete(NULL);
}

/* ---- the pages ---- */

static esp_err_t h_page(httpd_req_t *r)
{
    if (!s_on) return httpd_resp_send_404(r);
    static char names[16][33];
    const char *list[16];
    int n = net_seen(names, 16);
    for (int i = 0; i < n; i++) list[i] = names[i];
    char *page = malloc(PAGE_MAX);
    if (!page) return httpd_resp_send_500(r);
    int len = setup_page(page, PAGE_MAX, list, n);
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = len > 0 ? httpd_resp_send(r, page, len) : httpd_resp_send_500(r);
    free(page);
    return e;
}

static esp_err_t h_save(httpd_req_t *r)
{
    if (!s_on) return httpd_resp_send_404(r);
    char body[512];
    int got = 0;
    while (got < r->content_len && got < (int)sizeof body - 1) {
        int k = httpd_req_recv(r, body + got, sizeof body - 1 - got);
        if (k <= 0) break;
        got += k;
    }
    body[got] = 0;
    char ssid[33], pass[64];
    if (!setup_form(body, ssid, sizeof ssid, pass, sizeof pass)) {
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", "/");
        return httpd_resp_send(r, NULL, 0);
    }
    if (s_save) s_save(ssid, pass);
    char *page = malloc(4096);
    if (!page) return httpd_resp_send_500(r);
    int len = setup_saved_page(page, 4096, ssid);
    httpd_resp_set_type(r, "text/html");
    esp_err_t e = httpd_resp_send(r, page, len > 0 ? len : 0);
    free(page);
    return e;
}

/* Anything else -- a phone checking for a sign-in page (generate_204,
   hotspot-detect.html ...) -- is sent to the setup page. */
static esp_err_t h_other(httpd_req_t *r)
{
    if (!s_on) return httpd_resp_send_404(r);
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    return httpd_resp_send(r, NULL, 0);
}

void portal_start(void (*on_save)(const char *ssid, const char *pass))
{
    s_save = on_save;
    httpd_handle_t srv = mcpd_server();
    if (srv && !s_registered) {
        httpd_uri_t page = { .uri = "/", .method = HTTP_GET, .handler = h_page };
        httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = h_save };
        httpd_uri_t other = { .uri = "/*", .method = HTTP_GET, .handler = h_other };     /* last: after /mcp */
        httpd_register_uri_handler(srv, &page);
        httpd_register_uri_handler(srv, &save);
        httpd_register_uri_handler(srv, &other);
        s_registered = true;
    }
    if (!s_on) {
        s_on = true;
        xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    }
}

void portal_stop(void) { s_on = false; }
