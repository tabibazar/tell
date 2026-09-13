#include "textwrap.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check_lines(const char *what, const char *in, size_t cols,
                        size_t max_lines, const char **want, size_t want_n)
{
    char out[16][TW_MAX_COLS + 1];
    size_t n = textwrap(in, cols, max_lines, out);
    if (n != want_n) {
        printf("FAIL %s: got %zu lines, want %zu\n", what, n, want_n);
        for (size_t i = 0; i < n; i++) printf("      [%zu] %s\n", i, out[i]);
        failures++;
        return;
    }
    for (size_t i = 0; i < want_n; i++) {
        if (strcmp(out[i], want[i]) != 0) {
            printf("FAIL %s: line %zu got \"%s\", want \"%s\"\n",
                   what, i, out[i], want[i]);
            failures++;
            return;
        }
    }
    printf("ok   %s\n", what);
}

/* The same, for the field-aware wrap. */
static void check_fields(const char *what, const char *in, size_t cols,
                         size_t max_lines, const char **want, size_t want_n)
{
    char out[16][TW_MAX_COLS + 1];
    size_t n = textwrap_fields(in, cols, max_lines, out);
    if (n != want_n) {
        printf("FAIL %s: got %zu lines, want %zu\n", what, n, want_n);
        for (size_t i = 0; i < n; i++) printf("      [%zu] %s\n", i, out[i]);
        failures++;
        return;
    }
    for (size_t i = 0; i < want_n; i++) {
        if (strcmp(out[i], want[i]) != 0) {
            printf("FAIL %s: line %zu got \"%s\", want \"%s\"\n",
                   what, i, out[i], want[i]);
            failures++;
            return;
        }
    }
    printf("ok   %s\n", what);
}

static void check(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    check_lines("empty input clears", "", 30, 8, NULL, 0);
    check_lines("null input clears", NULL, 30, 8, NULL, 0);

    { const char *w[] = {"hi"};
      check_lines("short string", "hi", 30, 8, w, 1); }

    { const char *w[] = {"abcde"};
      check_lines("exact width", "abcde", 5, 8, w, 1); }

    { const char *w[] = {"hello", "world"};
      check_lines("word wrap", "hello world", 5, 8, w, 2); }

    { const char *w[] = {"abcd", "efgh", "ij"};
      check_lines("long word broken", "abcdefghij", 4, 8, w, 3); }

    { const char *w[] = {"aaaa", "b..."};
      check_lines("overflow marked", "aaaa bbbb cccc", 4, 2, w, 2); }

    { const char *w[] = {"a??b"};
      check_lines("non-ascii becomes ?", "a\xc3\xa9""b", 10, 8, w, 1); }

    { const char *w[] = {"one", "two"};
      check_lines("newline forces break", "one\ntwo", 10, 8, w, 2); }

    /*
     * Field-aware wrapping, for the lines the Mac sends as fields rather than
     * as prose. The weather is the one that matters: on lilly's twenty-six
     * columns, wrapping it as prose split "feels" from "27C" and "lo" from
     * "16", which reads as damage even when every character is present -- and
     * before that, a hand-rolled two-line wrap dropped the day's low off the
     * right-hand edge entirely, twice, with nothing to show it had.
     */
    {
        const char *wx = "Partly cloudy 26.7C  feels 27C  48%  11km/h  hi 27 lo 16";

        { const char *w[] = {"Partly cloudy 26.7C", "feels 27C 48% 11km/h", "hi 27 lo 16"};
          check_fields("the weather breaks between fields", wx, 26, 3, w, 3); }

        { const char *w[] = {"Partly cloudy 26.7C feels 27C 48% 11km/h hi 27 lo 16"};
          check_fields("a wide panel keeps it on one line", wx, 64, 3, w, 1); }

        /* Too little room must be marked, not hidden: a panel with only two
           rows for this cannot show the low, and must say so. */
        {
            char out[16][TW_MAX_COLS + 1];
            size_t n = textwrap_fields(wx, 26, 2, out);
            check("a panel with two rows says it is short", n == 2
                  && strstr(out[1], "...") != NULL);
            check("and the marked line still fits", strlen(out[1]) <= 26);
        }

        /* Prose with no field separators, and a field too long to fit, both
           fall back to ordinary wrapping rather than doing something worse. */
        { const char *w[] = {"hello", "world"};
          check_fields("no fields falls back to word wrap", "hello world", 5, 8, w, 2); }
        {
            char out[16][TW_MAX_COLS + 1];
            size_t n = textwrap_fields("Thunderstormwithhail 18C  gusting 60km/h", 12, 8, out);
            char joined[256] = "";
            for (size_t i = 0; i < n; i++) strcat(joined, out[i]);
            check("an overlong field falls back and keeps the tail",
                  strstr(joined, "60km/h") != NULL);
        }

        { check("empty input gives no lines",
                textwrap_fields("", 26, 3, (char (*)[TW_MAX_COLS + 1])0) == 0); }
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
