#include "mcpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mdns.h"

static const char *TAG = "mcpd";
#define BODY_MAX (400 * 1024)     /* a 200 KB JPEG in base64, and room */

static const mcp_tools_t *s_tools;
static char s_session[33];

static bool authorized(httpd_req_t *r)
{
    char h[CFG_VAL + 16] = "", tok[CFG_VAL];
    httpd_req_get_hdr_value_str(r, "Authorization", h, sizeof h);
    cfg_get(CFG_SERVE_TOKEN, tok, sizeof tok);
    if (mcp_auth_ok(h, tok)) return true;
    ESP_LOGW(TAG, "refused a request without the right token");
    httpd_resp_set_status(r, "401 Unauthorized");
    httpd_resp_set_hdr(r, "WWW-Authenticate", "Bearer");
    httpd_resp_send(r, NULL, 0);
    return false;
}

static esp_err_t post_mcp(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    if (r->content_len == 0 || r->content_len > BODY_MAX) {
        httpd_resp_set_status(r, "413 Payload Too Large");
        return httpd_resp_send(r, NULL, 0);
    }
    char *body = heap_caps_malloc(r->content_len + 1, MALLOC_CAP_SPIRAM);
    if (!body) return httpd_resp_send_500(r);
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, body + got, r->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(body); return ESP_FAIL; }
        got += (size_t)n;
    }
    body[got] = 0;
    char *resp = NULL;
    bool init = false;
    int code = mcp_rpc_handle(s_tools, body, got, &resp, &init);
    free(body);
    if (init) httpd_resp_set_hdr(r, "Mcp-Session-Id", s_session);
    if (code == 202) {
        httpd_resp_set_status(r, "202 Accepted");
        httpd_resp_send(r, NULL, 0);
    } else {
        httpd_resp_set_status(r, code == 200 ? "200 OK" : "400 Bad Request");
        httpd_resp_set_type(r, "application/json");
        httpd_resp_send(r, resp ? resp : "{}", HTTPD_RESP_USE_STRLEN);
    }
    free(resp);
    return ESP_OK;
}

static esp_err_t get_mcp(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    httpd_resp_set_status(r, "405 Method Not Allowed");       /* no server-to-client stream */
    httpd_resp_set_hdr(r, "Allow", "POST, DELETE");
    return httpd_resp_send(r, NULL, 0);
}

static esp_err_t delete_mcp(httpd_req_t *r)
{
    if (!authorized(r)) return ESP_OK;
    return httpd_resp_send(r, NULL, 0);
}

void mcpd_start(const mcp_tools_t *tools)
{
    s_tools = tools;
    for (int i = 0; i < 16; i++) snprintf(s_session + 2 * i, 3, "%02x", (unsigned)(esp_random() & 0xFF));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16384;      /* TLS (ElevenLabs, Deepgram) runs on this task in speak/listen/ask */
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    /* esp_http_server runs every URI handler on its single server task.
       mcp_rpc's call_tool keeps a static output buffer and relies on that
       (Task 4 ruling): keep this one task -- no async handlers or
       httpd_queue_work hand-offs that would run a handler elsewhere. */
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "HTTP server failed"); return; }
    httpd_uri_t post = { .uri = "/mcp", .method = HTTP_POST, .handler = post_mcp };
    httpd_uri_t get = { .uri = "/mcp", .method = HTTP_GET, .handler = get_mcp };
    httpd_uri_t del = { .uri = "/mcp", .method = HTTP_DELETE, .handler = delete_mcp };
    httpd_register_uri_handler(srv, &post);
    httpd_register_uri_handler(srv, &get);
    httpd_register_uri_handler(srv, &del);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("stan-claw");
        mdns_instance_name_set("stan-claw MCP server");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    ESP_LOGI(TAG, "MCP server at http://stan-claw.local/mcp%s", cfg_has(CFG_SERVE_TOKEN) ? "" : " (no serve token set: it refuses everyone)");
}
