/* spy's schedule and time stamp. Walks a whole week minute by minute through
   sched.c and counts; renders a stamped grey frame to renders/spy/stamp.pgm. */
#include "../spy/main/sched.h"
#include "../spy/main/stamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    struct tm s = { .tm_year = 126, .tm_mon = 9, .tm_mday = 4, .tm_hour = 0, .tm_isdst = -1 };  /* Sun 4 Oct 2026 */
    time_t t0 = mktime(&s);
    int frames[7] = { 0 }, quiet[7] = { 0 }, clips[7] = { 0 }, nights[7] = { 0 }, first_clip = -1, last_clip = -1;
    for (time_t t = t0; t < t0 + 7 * 86400; t += 60) {
        struct tm lt;
        localtime_r(&t, &lt);
        if (sched_capture(&lt)) frames[lt.tm_wday]++;
        if (sched_capture_quiet(&lt)) { quiet[lt.tm_wday]++; CHECK(!sched_capture(&lt) && lt.tm_min % 10 == 0); }
        time_t nf;
        if (sched_night_clip(&lt, &nf)) {
            nights[lt.tm_wday]++;
            struct tm fl;
            localtime_r(&nf, &fl);
            CHECK(fl.tm_hour == 17 && fl.tm_min == 0);
            CHECK(fl.tm_wday == (lt.tm_wday == 1 ? 5 : lt.tm_wday - 1));
            double hours = difftime(t, nf) / 3600.0;
            CHECK(lt.tm_wday == 1 ? hours == 62 : hours == 14);
        }
        if (sched_day_clip(&lt)) {
            clips[lt.tm_wday]++;
            if (first_clip < 0) first_clip = lt.tm_hour;
            last_clip = lt.tm_hour;
        }
    }
    CHECK(frames[0] == 0 && frames[6] == 0);           /* weekends off */
    for (int d = 1; d <= 5; d++) { CHECK(frames[d] == 600); CHECK(clips[d] == 1); }
    CHECK(clips[0] == 0 && clips[6] == 0);
    for (int d = 1; d <= 5; d++) CHECK(quiet[d] == 14 * 6);      /* 17:00-06:59 */
    for (int d = 1; d <= 5; d++) CHECK(nights[d] == 1);
    CHECK(nights[0] == 0 && nights[6] == 0);
    CHECK(quiet[0] == 24 * 6 && quiet[6] == 24 * 6);
    CHECK(first_clip == 17 && last_clip == 17);
    struct tm d = { .tm_year = 126, .tm_mon = 9, .tm_mday = 2 };
    char day[9];
    sched_day(&d, day);
    CHECK(strcmp(day, "20261002") == 0);

    const int w = 640, h = 480;
    unsigned char *f = malloc(w * h * 3 / 2);
    memset(f, 150, w * h); memset(f + w * h, 100, w * h / 2);
    stamp_time(f, w, h, 7, 42);
    stamp_time(f, w, h, 25, 0);                         /* refused, no crash */
    CHECK(f[0] == 150);
    CHECK(f[(h - 20) * w + (w - 30)] != 150);           /* the box is in the corner */
    FILE *o = fopen("host_tests/renders/spy/stamp.pgm", "wb");
    if (o) { fprintf(o, "P5 %d %d 255\n", w, h); fwrite(f, 1, w * h, o); fclose(o); }
    free(f);
    printf(fails ? "%d FAILED\n" : "spysched: all passed\n", fails);
    return fails != 0;
}
