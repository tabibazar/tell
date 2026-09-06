#include "usagedata.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    usagedata_t d;
    memset(&d, 0, sizeof d);

    expect("plain text is not data", usagedata_parse(&d, "hello world") == UD_NONE);
    expect("unknown marker is not data", usagedata_parse(&d, "!nope\nx 1") == UD_NONE);
    expect("NULL is not data", usagedata_parse(&d, NULL) == UD_NONE);

    expect("stats marker recognised",
           usagedata_parse(&d, "!stats\nm opus-5 13000000 3300000000\n"
                               "m sonnet-5 1200000 429000000\n") == UD_STATS);
    expect("two models parsed", d.model_count == 2);
    expect("model name kept", strcmp(d.models[0].name, "opus-5") == 0);
    expect("output tokens parsed", d.models[0].out == 13000000ULL);
    expect("cache tokens exceed 32 bits", d.models[0].cread == 3300000000ULL);

    expect("daily marker recognised",
           usagedata_parse(&d, "!daily\nd 09-01 571000000\nd 09-02 282000000\n") == UD_DAILY);
    expect("two days parsed", d.day_count == 2);
    expect("day label kept", strcmp(d.days[0].label, "09-01") == 0);
    expect("day tokens parsed", d.days[1].tokens == 282000000ULL);
    expect("stats survived the daily parse", d.model_count == 2);

    usagedata_parse(&d, "!stats\nm a 1 2\ngarbage line\nm b 3 4\n");
    expect("a bad line does not lose good rows", d.model_count == 2);

    usagedata_parse(&d, "!stats\nm only-name\n");
    expect("a row missing fields is skipped", d.model_count == 0);

    usagedata_parse(&d, "!stats\nm trunc 5");
    expect("a payload truncated mid-line still parses", d.model_count == 1);

    char many[1024] = "!stats\n";
    for (int i = 0; i < UD_MAX_MODELS + 4; i++) {
        char row[40];
        snprintf(row, sizeof row, "m mdl%d %d %d\n", i, i, i);
        strncat(many, row, sizeof many - strlen(many) - 1);
    }
    usagedata_parse(&d, many);
    expect("model count is capped", d.model_count == UD_MAX_MODELS);

    usagedata_parse(&d, "!stats\nm verylongmodelnamethatoverflows 1 2\n");
    expect("long name is truncated, not overflowed",
           strlen(d.models[0].name) <= UD_NAME_MAX);

    usagedata_parse(&d, "!stats\n");
    expect("empty section yields no rows", d.model_count == 0);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
