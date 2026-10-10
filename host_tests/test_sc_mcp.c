/* stan-claw's MCP server core: JSON-RPC in, JSON-RPC out, tools stubbed. */
#include "mcp_rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char last[256];
static int last_int;
static size_t last_len;
static bool speak(const char *t, char *o, size_t n) { snprintf(last, sizeof last, "speak:%s", t); snprintf(o, n, "spoken"); return true; }
static bool listen_(int s, char *o, size_t n) { last_int = s; snprintf(o, n, "hello there"); return true; }
static bool ask(const char *q, char *o, size_t n) { snprintf(last, sizeof last, "ask:%s", q); snprintf(o, n, "yes"); return true; }
static bool show_text(const char *ti, const char *t, char *o, size_t n) { snprintf(last, sizeof last, "%s|%s", ti ? ti : "-", t); snprintf(o, n, "shown"); return true; }
static bool show_image(const uint8_t *j, size_t len, char *o, size_t n) { (void)j; last_len = len; snprintf(o, n, "shown"); return true; }
static bool clear_(char *o, size_t n) { snprintf(o, n, "cleared"); return true; }
static bool vol(int l, char *o, size_t n) { last_int = l; snprintf(o, n, "%d", l); return true; }
static bool status_(char *o, size_t n) { snprintf(o, n, "busy"); return false; }   /* a tool error */
static const mcp_tools_t T = { speak, listen_, ask, show_text, show_image, clear_, vol, status_ };

static char *call(const char *body, int *code, bool *init)
{
    char *r = NULL;
    bool i = false;
    *code = mcp_rpc_handle(&T, body, strlen(body), &r, &i);
    if (init) *init = i;
    return r;
}

int main(void)
{
    int code;
    bool init;
    char *r = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}", &code, &init);
    CHECK(code == 200 && init && strstr(r, "\"protocolVersion\":\"2025-06-18\"") && strstr(r, "\"name\":\"stan-claw\"") && strstr(r, "\"id\":1"));
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &code, NULL);
    CHECK(code == 202 && r == NULL);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"tools/list\"}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"id\":\"a\""));
    const char *names[] = { "speak", "listen", "ask", "show_text", "show_image", "clear_screen", "set_volume", "status" };
    for (int i = 0; i < 8; i++) { char q[40]; snprintf(q, sizeof q, "\"name\":\"%s\"", names[i]); CHECK(strstr(r, q) != NULL); }
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"speak\",\"arguments\":{\"text\":\"hi\"}}}", &code, NULL);
    CHECK(code == 200 && strcmp(last, "speak:hi") == 0 && strstr(r, "\"text\":\"spoken\"") && !strstr(r, "\"isError\":true"));
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"listen\",\"arguments\":{}}}", &code, NULL);
    CHECK(code == 200 && last_int == 10 && strstr(r, "hello there"));     /* default 10 s */
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"listen\",\"arguments\":{\"max_seconds\":99}}}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"isError\":true"));                  /* out of range */
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"show_text\",\"arguments\":{\"text\":\"t\",\"title\":\"T\"}}}", &code, NULL);
    CHECK(code == 200 && strcmp(last, "T|t") == 0);
    free(r);

    /* "/9j/4AAQ" is base64 of FF D8 FF E0 00 10: a JPEG's start. */
    r = call("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"show_image\",\"arguments\":{\"jpeg_base64\":\"/9j/4AAQ\"}}}", &code, NULL);
    CHECK(code == 200 && last_len == 6 && !strstr(r, "\"isError\":true"));
    free(r);
    r = call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"show_image\",\"arguments\":{\"jpeg_base64\":\"aGVsbG8=\"}}}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"isError\":true"));                  /* "hello" is not a JPEG */
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"set_volume\",\"arguments\":{\"level\":40}}}", &code, NULL);
    CHECK(code == 200 && last_int == 40);
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"status\",\"arguments\":{}}}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"isError\":true") && strstr(r, "busy"));
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"fly\",\"arguments\":{}}}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"code\":-32602"));
    free(r);

    r = call("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"resources/list\"}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"code\":-32601"));
    free(r);

    r = call("{nope", &code, NULL);
    CHECK(code == 400 && strstr(r, "\"code\":-32700"));
    free(r);
    r = call("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]", &code, NULL);
    CHECK(code == 400 && strstr(r, "\"code\":-32600"));                    /* no batches in 2025-06-18 */
    free(r);
    r = call("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"ping\"}", &code, NULL);
    CHECK(code == 200 && strstr(r, "\"result\":{}"));
    free(r);

    CHECK(mcp_auth_ok("Bearer s3cret", "s3cret"));
    CHECK(!mcp_auth_ok("Bearer s3cre", "s3cret"));
    CHECK(!mcp_auth_ok("s3cret", "s3cret"));
    CHECK(!mcp_auth_ok(NULL, "s3cret"));
    CHECK(!mcp_auth_ok("Bearer ", ""));                                     /* no token set: nobody gets in */

    uint8_t b[8];
    CHECK(mcp_b64_decode("aGVsbG8=", b, sizeof b) == 5 && memcmp(b, "hello", 5) == 0);
    CHECK(mcp_b64_decode("aGVsbG8=", b, 4) == 0);                           /* would overflow */
    CHECK(mcp_b64_decode("a*==", b, sizeof b) == 0);                        /* not base64 */

    printf(fails ? "%d FAILED\n" : "sc_mcp: all passed\n", fails);
    return fails != 0;
}
