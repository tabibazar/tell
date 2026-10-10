/* stan-claw's care logic: the JSON-RPC body/reply for its own calls to
   stan-mcp, reminders and family messages, and when it may speak on its
   own. */
#include "care.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* The RPC body: a tools/call envelope around the given arguments. */
    char *body = care_rpc_body("reminders", "{\"homeowner_token\":\"hw_abc\"}");
    cJSON *j = cJSON_Parse(body);
    CHECK(j != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(j, "jsonrpc")->valuestring, "2.0") == 0);
    CHECK(cJSON_GetObjectItem(j, "id")->valueint == 1);
    CHECK(strcmp(cJSON_GetObjectItem(j, "method")->valuestring, "tools/call") == 0);
    cJSON *params = cJSON_GetObjectItem(j, "params");
    CHECK(strcmp(cJSON_GetObjectItem(params, "name")->valuestring, "reminders") == 0);
    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    CHECK(args && strcmp(cJSON_GetObjectItem(args, "homeowner_token")->valuestring, "hw_abc") == 0);
    cJSON_Delete(j);
    free(body);
    CHECK(care_rpc_body("x", "not json") == NULL);
    CHECK(care_rpc_body("x", "[1,2]") == NULL);               /* an array, not an object */
    CHECK(care_rpc_body("x", NULL) == NULL);

    /* The reply's text, real and an error. */
    char out[200];
    CHECK(care_rpc_text("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":"
                         "[{\"type\":\"text\",\"text\":\"{\\\"reminders\\\":[]}\"}]}}", out, sizeof out));
    CHECK(strcmp(out, "{\"reminders\":[]}") == 0);
    CHECK(!care_rpc_text("{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,\"message\":\"nope\"}}", out, sizeof out));
    CHECK(!care_rpc_text("not even json", out, sizeof out));
    CHECK(!care_rpc_text("{\"result\":{}}", out, sizeof out));            /* no content */

    /* Reminders: parsed fresh each time, but a done one stays done. */
    static care_list_t l;
    l.n = 0;
    const char *rem1 = "{\"reminders\":[{\"id\":\"trash\",\"at\":1000,\"say\":\"Trash bins go out tonight\"},"
                       "{\"id\":\"dues\",\"at\":2000,\"say\":\"Dues are due in 3 days\"}]}";
    CHECK(care_parse_reminders(rem1, &l) == 2 && l.n == 2 && !l.item[0].done);
    l.item[0].done = true;                                     /* "trash" was said */
    const char *rem2 = "{\"reminders\":[{\"id\":\"trash\",\"at\":1000,\"say\":\"Trash bins go out tonight\"},"
                       "{\"id\":\"dues\",\"at\":2000,\"say\":\"Dues are due in 3 days\"},"
                       "{\"id\":\"meeting\",\"at\":3000,\"say\":\"Board meeting tomorrow\"}]}";
    CHECK(care_parse_reminders(rem2, &l) == 3 && l.n == 3);
    CHECK(l.item[0].done && !l.item[1].done && !l.item[2].done);          /* trash still done, the rest fresh */
    CHECK(care_parse_reminders("garbage", &l) == -1);
    CHECK(care_parse_reminders("{\"nope\":1}", &l) == -1);
    CHECK(l.n == 3);                                            /* junk leaves the list alone */

    /* Messages: appended, deduplicated by id, oldest dropped past CARE_MAX. */
    static care_list_t msgs;
    msgs.n = 0;
    const char *m1 = "{\"messages\":[{\"id\":\"m1\",\"from\":\"Reza\",\"text\":\"Hi dad\",\"at\":500}]}";
    CHECK(care_parse_messages(m1, &msgs) == 1 && msgs.n == 1);
    CHECK(strcmp(msgs.item[0].from, "Reza") == 0 && strcmp(msgs.item[0].text, "Hi dad") == 0 && msgs.item[0].at == 500);
    CHECK(care_parse_messages(m1, &msgs) == 0 && msgs.n == 1);            /* same id again: not re-added */
    const char *m2 = "{\"messages\":[{\"id\":\"m1\",\"from\":\"Reza\",\"text\":\"Hi dad\",\"at\":500},"
                     "{\"id\":\"m2\",\"from\":\"Reza\",\"text\":\"Call me\",\"at\":600}]}";
    CHECK(care_parse_messages(m2, &msgs) == 1 && msgs.n == 2);            /* only m2 is new */
    for (int i = 0; i < CARE_MAX + 3; i++) {
        char one[200];
        snprintf(one, sizeof one, "{\"messages\":[{\"id\":\"g%d\",\"from\":\"Reza\",\"text\":\"x\",\"at\":%d}]}", i, 1000 + i);
        care_parse_messages(one, &msgs);
    }
    CHECK(msgs.n == CARE_MAX);
    CHECK(strcmp(msgs.item[CARE_MAX - 1].id, "g14") == 0);                /* the latest kept */
    CHECK(care_parse_messages("junk", &msgs) == -1);

    /* Speaking hours: 09:00 up to but not including 19:00. */
    CHECK(!care_may_speak(8 * 60 + 59));
    CHECK(care_may_speak(9 * 60));
    CHECK(care_may_speak(18 * 60 + 59));
    CHECK(!care_may_speak(19 * 60));

    /* Due: not done, at or before now, less than 6 h stale, in the window;
       the earliest due one wins. */
    static care_list_t due;
    due.n = 0;
    strcpy(due.item[0].id, "a"); due.item[0].at = 1000; due.item[0].done = false;
    strcpy(due.item[1].id, "b"); due.item[1].at = 500;  due.item[1].done = false;
    strcpy(due.item[2].id, "c"); due.item[2].at = 900;  due.item[2].done = true;      /* already said */
    strcpy(due.item[3].id, "d"); due.item[3].at = 1500; due.item[3].done = false;     /* in the future */
    strcpy(due.item[4].id, "e"); due.item[4].at = 500 - 6 * 3600; due.item[4].done = false;  /* stale */
    due.n = 5;
    int noon = 12 * 60;
    int idx = care_due(&due, 1000, noon);
    CHECK(idx == 1 && strcmp(due.item[idx].id, "b") == 0);                /* earliest: "b" at 500 */
    CHECK(care_due(&due, 1000, 8 * 60 + 59) == -1);                      /* too early to speak */
    CHECK(care_due(&due, 1000, 19 * 60) == -1);                          /* too late to speak */
    due.item[0].done = true;
    due.item[1].done = true;
    CHECK(care_due(&due, 1000, noon) == -1);                             /* only the stale and future ones left */
    static care_list_t stale;
    stale.n = 1;
    strcpy(stale.item[0].id, "s"); stale.item[0].done = false;
    stale.item[0].at = 10000 - 6 * 3600;                                  /* exactly 6 h ago: too stale */
    CHECK(care_due(&stale, 10000, noon) == -1);
    stale.item[0].at = 10000 - 6 * 3600 + 1;                              /* one second under 6 h: still due */
    CHECK(care_due(&stale, 10000, noon) == 0);

    /* Next wake: the earliest not-done reminder still ahead; 0 if none. */
    CHECK(care_next_wake(&due, 1000) == 1500);                           /* "d" */
    static care_list_t none_ahead;
    none_ahead.n = 2;
    strcpy(none_ahead.item[0].id, "x"); none_ahead.item[0].at = 100; none_ahead.item[0].done = false;
    strcpy(none_ahead.item[1].id, "y"); none_ahead.item[1].at = 2000; none_ahead.item[1].done = true;  /* done: skipped */
    CHECK(care_next_wake(&none_ahead, 1000) == 0);

    /* The list round-trips through JSON, done flags and all. */
    static care_list_t a, b;
    a.n = 2;
    strcpy(a.item[0].id, "r1"); a.item[0].at = 111; a.item[0].from[0] = 0;
    strcpy(a.item[0].text, "Trash night"); a.item[0].done = true;
    strcpy(a.item[1].id, "m9"); a.item[1].at = 222; strcpy(a.item[1].from, "Reza");
    strcpy(a.item[1].text, "Call me back"); a.item[1].done = false;
    char *js = care_list_json(&a);
    CHECK(js && care_list_load(js, &b));
    CHECK(b.n == 2 && strcmp(b.item[0].id, "r1") == 0 && b.item[0].done && b.item[0].from[0] == 0);
    CHECK(strcmp(b.item[1].from, "Reza") == 0 && !b.item[1].done && b.item[1].at == 222);
    free(js);
    CHECK(!care_list_load("not json", &b));
    CHECK(!care_list_load("{\"nope\":1}", &b));

    printf(fails ? "%d FAILED\n" : "sc_care: all passed\n", fails);
    return fails != 0;
}
