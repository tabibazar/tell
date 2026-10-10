#include "cmdline.h"

#include <stddef.h>

static int space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int cmd_split(const char *s, char w[][CMD_WORD], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (space(*s)) s++;
        if (!*s) break;
        size_t o = 0;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q) {
                if (o + 1 < CMD_WORD) w[n][o++] = *s;
                s++;
            }
            if (*s == q) s++;
        } else {
            while (*s && !space(*s)) {
                if (o + 1 < CMD_WORD) w[n][o++] = *s;
                s++;
            }
        }
        w[n][o] = 0;
        n++;
    }
    return n;
}
