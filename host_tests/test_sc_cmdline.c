/* stan-claw's console: a line into words, quotes keeping spaces. */
#include "cmdline.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    char w[CMD_MAX][CMD_WORD];
    CHECK(cmd_split("wifi add \"Office WiFi\" 'p a ss'\r\n", w, CMD_MAX) == 4);
    CHECK(strcmp(w[0], "wifi") == 0 && strcmp(w[2], "Office WiFi") == 0 && strcmp(w[3], "p a ss") == 0);
    CHECK(cmd_split("   ", w, CMD_MAX) == 0);
    CHECK(cmd_split("key claude sk-abc", w, 2) == 2);              /* stops at max */
    CHECK(cmd_split("mcp url https://x.example/mcp?a=1&b=2", w, CMD_MAX) == 3);
    CHECK(strcmp(w[2], "https://x.example/mcp?a=1&b=2") == 0);
    CHECK(cmd_split("serve token \"\"", w, CMD_MAX) == 3 && w[2][0] == 0);   /* an empty quoted word */
    char longw[CMD_WORD + 50];
    memset(longw, 'k', sizeof longw - 1);
    longw[sizeof longw - 1] = 0;
    CHECK(cmd_split(longw, w, CMD_MAX) == 1 && strlen(w[0]) == CMD_WORD - 1);   /* cut, not overrun */
    printf(fails ? "%d FAILED\n" : "sc_cmdline: all passed\n", fails);
    return fails != 0;
}
