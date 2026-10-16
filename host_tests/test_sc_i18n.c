/* stan-claw's languages: every word in every language there, each format
   with the English one's %s, every letter in the faces that draw it, the
   friendly day and the weather words in each. */
#include "i18n.h"
#include "aafont.h"
#include "weather.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

extern const aafont_t aafont_sc_head, aafont_sc_title, aafont_sc_small, aafont_sc_sub, aafont_sc_answer_xl;

static int percents(const char *s)
{
    int n = 0;
    for (; *s; s++) if (*s == '%') n++;
    return n;
}

/* The next code point of valid UTF-8, or -1. */
static long next_cp(const unsigned char **p)
{
    const unsigned char *s = *p;
    if (s[0] < 0x80) { *p = s + 1; return s[0]; }
    if (s[0] >= 0xC2 && s[0] <= 0xDF && (s[1] & 0xC0) == 0x80) { *p = s + 2; return (s[0] & 0x1F) << 6 | (s[1] & 0x3F); }
    if (s[0] >= 0xE0 && s[0] <= 0xEF && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *p = s + 3;
        return (s[0] & 0x0F) << 12 | (s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    }
    *p = s + 1;
    return -1;
}

static int has(const aafont_t *f, long cp)
{
    for (int i = 0; i < f->count; i++) if (f->glyphs[i].cp == cp) return 1;
    return 0;
}

/* Every character of s is valid UTF-8 and in every face stan-claw draws words with. */
static int drawable(const char *s)
{
    const aafont_t *faces[5] = { &aafont_sc_head, &aafont_sc_title, &aafont_sc_small, &aafont_sc_sub, &aafont_sc_answer_xl };
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        long cp = next_cp(&p);
        if (cp < 0) { printf("  bad UTF-8 in \"%s\"\n", s); return 0; }
        for (int f = 0; f < 5; f++)
            if (!has(faces[f], cp)) { printf("  U+%04lX missing from a face, in \"%s\"\n", cp, s); return 0; }
    }
    return 1;
}

int main(void)
{
    /* Codes. */
    CHECK(lang_find("") == LANG_EN && lang_find("es") == LANG_ES && lang_find("tl") == LANG_TL);
    CHECK(lang_find("xx") == -1 && lang_find("ES") == -1 && lang_from_code("xx") == LANG_EN);
    for (int l = 0; l < LANG_N; l++) CHECK(lang_find(lang_code(l)) == l);
    CHECK(strcmp(lang_name(LANG_ES), "Español") == 0 && strcmp(lang_name(LANG_PT), "Português") == 0);
    CHECK(strcmp(lang_english(LANG_DE), "German") == 0 && strcmp(lang_name(-3), "English") == 0);

    /* Every key in every language, drawable, with the English one's format. */
    for (int l = 0; l < LANG_N; l++) {
        CHECK(drawable(lang_name(l)));
        for (int k = 0; k < TR_N; k++) {
            const char *s = tr(l, k);
            if (!s[0]) printf("  empty: lang %d key %d\n", l, k);
            CHECK(s[0]);
            CHECK(drawable(s));
            if (percents(s) != percents(tr(LANG_EN, k))) printf("  format: lang %d key %d \"%s\"\n", l, k, s);
            CHECK(percents(s) == percents(tr(LANG_EN, k)));
        }
    }
    CHECK(strcmp(tr(LANG_EN, TR_TALK), "Talk") == 0 && strcmp(tr(LANG_ES, TR_TALK), "Hablar") == 0);
    CHECK(strcmp(tr(99, TR_TALK), "Talk") == 0 && strcmp(tr(LANG_DE, TR_N), "") == 0);

    /* The friendly day. */
    char part[48], date[48];
    tr_day_words(LANG_EN, 6, 10, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday afternoon") == 0 && strcmp(date, "October 10") == 0);
    tr_day_words(LANG_ES, 6, 10, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Sábado por la tarde") == 0 && strcmp(date, "10 de octubre") == 0);
    tr_day_words(LANG_FR, 3, 8, 14, 8 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Mercredi matin") == 0 && strcmp(date, "14 août") == 0);
    tr_day_words(LANG_PT, 1, 3, 2, 19 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Segunda-feira à noite") == 0 && strcmp(date, "2 de março") == 0);
    tr_day_words(LANG_IT, 5, 12, 25, 23 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Venerdì notte") == 0 && strcmp(date, "25 dicembre") == 0);
    tr_day_words(LANG_DE, 6, 10, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Samstagnachmittag") == 0 && strcmp(date, "10. Oktober") == 0);
    tr_day_words(LANG_TL, 0, 1, 1, 7 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Linggo ng umaga") == 0 && strcmp(date, "Enero 1") == 0);
    tr_day_words(LANG_ES, 6, 10, 0, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(date, "Octubre") == 0);                                  /* a month alone starts the line */
    tr_day_words(LANG_FR, 6, 0, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(part[0] == 0 && date[0] == 0);
    for (int l = 0; l < LANG_N; l++)
        for (int d = 0; d < 7; d++)
            for (int mo = 1; mo <= 12; mo++) {
                tr_day_words(l, d, mo, 28, (d * 200 + mo * 100) % 1440, part, sizeof part, date, sizeof date);
                CHECK(drawable(part) && drawable(date) && part[0] && date[0]);
            }

    /* The weather words. */
    CHECK(strcmp(tr_weather(LANG_EN, 2), "Partly cloudy") == 0 && strcmp(tr_weather(LANG_ES, 63), "Lluvia") == 0);
    CHECK(strcmp(tr_weather(LANG_DE, 95), "Gewitter") == 0 && strcmp(tr_weather(LANG_FR, 66), "Pluie verglaçante") == 0);
    static const int codes[] = { 0, 1, 2, 3, 45, 51, 61, 63, 65, 66, 71, 73, 95, 99, 42 };   /* 42: Mixed */
    for (int l = 0; l < LANG_N; l++)
        for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
            const char *w = tr_weather(l, codes[i]);
            CHECK(drawable(w) && (l == LANG_EN || strcmp(w, weather_words(codes[i])) != 0));
        }

    printf(fails ? "%d FAILED\n" : "sc_i18n: all passed\n", fails);
    return fails != 0;
}
