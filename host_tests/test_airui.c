/* blinky1's air log and OLED pages: rolls a week of synthetic minutes through
   airlog, checks the roll-ups and the save/load round trip, and renders every
   page to host_tests/renders/airui/ as PBM. */
#include "../blinky1/main/airlog.h"
#include "../blinky1/main/airui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static airlog_t L, L2;

static void pbm(const uint8_t *fb, const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "host_tests/renders/airui/%s.pbm", name);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P1\n128 64\n");
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++) fputs(fb[(y >> 3) * 128 + x] & (1 << (y & 7)) ? "1 " : "0 ", o);
        fputc('\n', o);
    }
    fclose(o);
}

int main(void)
{
    airlog_init(&L);
    int quarters = 0;
    for (int m = 0; m < 9 * 24 * 60; m++) {            /* nine days of minutes */
        float day = sinf(m * 6.2832f / 1440.0f);
        air_point_t p = { { (int16_t)(240 + 20 * day), (int16_t)(400 + 60 * day),
                            (int16_t)(500 + 300 * fmaxf(0, day) + (m % 7) * 3), (int16_t)(60 + 40 * fmaxf(0, day)) } };
        if (m > 3000 && m < 3060) p.v[AIR_ECO2] = AIRLOG_NONE;   /* a gap */
        quarters += airlog_minute(&L, &p);
    }
    CHECK(quarters == 9 * 24 * 4, "quarters %d", quarters);
    CHECK(L.hour_n == AIRLOG_HOUR && L.day_n == AIRLOG_DAY && L.week_n == AIRLOG_WEEK, "rings %d %d %d", L.hour_n, L.day_n, L.week_n);
    int16_t s[AIRLOG_DAY];
    int n = airlog_series(&L, AIR_TEMP, AIR_RANGE_DAY, s);
    int lo = 9999, hi = -9999;
    for (int i = 0; i < n; i++) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
    CHECK(n == AIRLOG_DAY && lo >= 218 && hi <= 262 && hi - lo > 30, "day series %d..%d", lo, hi);

    uint8_t buf[4096];
    size_t len = airlog_save(&L, buf, sizeof buf);
    CHECK(len > 0 && airlog_load(&L2, buf, len), "save/load");
    int16_t s2[AIRLOG_DAY];
    CHECK(airlog_series(&L2, AIR_TEMP, AIR_RANGE_DAY, s2) == n && !memcmp(s, s2, sizeof s2), "day survives");
    CHECK(airlog_series(&L2, AIR_TEMP, AIR_RANGE_HOUR, s2) == 0, "hour starts empty after load");
    buf[0] ^= 1;
    CHECK(!airlog_load(&L2, buf, len), "bad magic refused");
    printf("saved %zu bytes\n", len);

    uint8_t fb[1024];
    air_now_t now = { 24.8f, 41.0f, 612, 87, 2, 0 };
    for (int p = 0; p < AIRUI_PAGES; p++) {
        airui_draw(fb, p, &now, &L);
        char name[16];
        snprintf(name, sizeof name, "p%02d", p);
        pbm(fb, name);
    }
    air_now_t warm = { 25.3f, 41.0f, 400, 24, 1, 2 };
    airui_draw(fb, 0, &warm, &L);
    pbm(fb, "now-warming");
    airlog_t empty; airlog_init(&empty);
    airui_draw(fb, 1, &now, &empty);
    pbm(fb, "empty-chart");
    uint8_t r, g, b;
    airui_colour(&now, &r, &g, &b);
    CHECK(g > r && b == 0, "good air is green-ish: %d %d %d", r, g, b);
    airui_colour(&warm, &r, &g, &b);
    CHECK(b > 0 && r == 0 && g == 0, "warming is blue");
    printf(fails ? "%d FAILED\n" : "airui: all passed\n", fails);
    return fails != 0;
}
