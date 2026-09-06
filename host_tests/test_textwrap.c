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

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
