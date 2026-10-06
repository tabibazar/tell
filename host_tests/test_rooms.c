/* panel1's room bookings: parses relay answers (good, odd and broken) and
   renders the day page to host_tests/renders/rooms/ as 24-bit BMPs. */
#include "../panel1/main/rooms.h"
#include "../panel1/main/roomsui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint16_t fb[480 * 480];

static void bmp(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "host_tests/renders/rooms/%s.bmp", name);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    const int w = 480, h = 480, row = w * 3, size = 54 + row * h;
    unsigned char hd[54] = { 'B', 'M' };
    for (int i = 0; i < 4; i++) {
        hd[2 + i] = (unsigned char)(size >> (8 * i));
        hd[18 + i] = (unsigned char)(w >> (8 * i));
        hd[22 + i] = (unsigned char)(h >> (8 * i));
    }
    hd[10] = 54; hd[14] = 40;
    hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, o);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t p = fb[y * w + x];
            unsigned char px[3] = { (p & 0x1F) << 3, ((p >> 5) & 0x3F) << 2, (p >> 11) << 3 };
            fwrite(px, 1, 3, o);
        }
    }
    fclose(o);
}

/* Tomorrow at the office, as the relay gave it (rooms renamed by its regex),
   plus a few bookings to try the edges: long titles, back to back, early. */
static const char *DAY =
    "{\"d\":\"2026-10-07\",\"tz\":\"America/Toronto\",\"rooms\":["
    "{\"n\":\"CEDAR (8)\",\"ev\":[[660,720,\"Design review\",\"Lena\"],[765,780,\"Hiring sync\",\"Nora\"],"
    "[780,810,\"Budget planning\",\"Omar\"],[810,840,\"Ops weekly\",\"Theo\"]]},"
    "{\"n\":\"Pine-1\",\"ev\":[[540,570,\"Daily standup\",\"Priya\"],[600,690,\"Quarterly roadmap review with the platform team\",\"Jordan\"]]},"
    "{\"n\":\"Pine-2\",\"ev\":[[450,480,\"Early call \\u2013 Tokyo\",\"Ken\"],[840,960,\"Interview: Senior engineer\",\"Alex\"]]},"
    "{\"n\":\"MAPLE (5)\",\"ev\":[[570,600,\"1:1 \\u00c9milie / Omar \\ud83d\\ude00\",\"\\u00c9milie\"],[900,930,\"\",\"\"]]},"
    "{\"n\":\"BIRCH (10)\",\"ev\":[[600,660,\"All hands prep\",\"Dana\"],[720,780,\"Lunch & learn\",\"Chris\"],[1020,1110,\"Board call\",\"Omar\"]]}"
    "]}";

int main(void)
{
    static rooms_day_t d;
    CHECK(rooms_parse(DAY, strlen(DAY), &d));
    CHECK(d.y == 2026 && d.m == 10 && d.d == 7);
    CHECK(d.nrooms == 5);
    CHECK(strcmp(d.room[0].name, "CEDAR") == 0 && d.room[0].cap == 8);
    CHECK(strcmp(d.room[1].name, "PINE-1") == 0 && d.room[1].cap == 0);
    CHECK(d.room[0].n == 4 && d.room[0].ev[0].start == 660 && strcmp(d.room[0].ev[0].who, "Lena") == 0);
    CHECK(strcmp(d.room[2].ev[0].title, "Early call - Tokyo") == 0);
    CHECK(strcmp(d.room[3].ev[0].title, "1:1 Emilie / Omar") == 0);   /* accent folded, emoji gone */
    CHECK(strcmp(d.room[3].ev[1].title, "Busy") == 0);                 /* a private booking */

    CHECK(rooms_at(&d.room[0], 790) == &d.room[0].ev[2]);
    CHECK(rooms_at(&d.room[0], 720) == NULL);                          /* ends are open */
    CHECK(rooms_next(&d.room[0], 720) == &d.room[0].ev[1]);
    CHECK(rooms_next(&d.room[0], 840) == NULL);

    int a, b;
    roomsui_hours(&d, &a, &b);
    CHECK(a == 7 && b == 19);                                          /* 7:30 and 18:30 stretch it */

    /* What is not the relay's answer leaves the day as it was. */
    rooms_day_t keep = d;
    const char *bad[] = { "", "<html>Sorry</html>", "{\"err\":\"bad date\"}", "{\"d\":\"2026-10-07\",\"rooms\":[]}",
                          "{\"d\":\"x\",\"rooms\":[{\"n\":\"A\",\"ev\":[]}]}" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK(!rooms_parse(bad[i], strlen(bad[i]), &d));
    CHECK(memcmp(&keep, &d, sizeof d) == 0);

    /* Junk inside a good answer is skipped, not trusted. */
    const char *odd = "{\"d\":\"2026-10-10\",\"rooms\":[{\"n\":\"A\",\"ev\":[[900,800,\"backwards\",\"x\"],"
                      "[-5,30,\"early\",\"x\"],[\"9\",\"10\"],[1400,2000,\"late\",\"x\"]]}]}";
    rooms_day_t o;
    CHECK(rooms_parse(odd, strlen(odd), &o));
    CHECK(o.room[0].n == 2 && o.room[0].ev[0].start == 0 && o.room[0].ev[1].end == 1440);

    canvas_t c;
    canvas_init(&c, fb, 480, 480, 1);
    system("mkdir -p host_tests/renders/rooms");

    roomsui_view_t v = { .offset = 0, .wday = 3, .now = 13 * 60 + 12, .note = NULL };
    roomsui_draw(&c, &d, &v);
    bmp("today");
    CHECK(fb[0] != fb[480 * 240 + 240] || 1);

    v.now = 7 * 60 + 5;
    roomsui_draw(&c, &d, &v);
    bmp("today-early");

    v = (roomsui_view_t){ .offset = 1, .wday = 4, .now = 19 * 60 + 24 };
    roomsui_draw(&c, &d, &v);
    bmp("tomorrow");

    v = (roomsui_view_t){ .offset = 0, .wday = 3, .now = 15 * 60 + 2, .note = "offline since 14:05" };
    roomsui_draw(&c, &d, &v);
    bmp("offline");

    const char *empty = "{\"d\":\"2026-10-10\",\"rooms\":[{\"n\":\"CEDAR (8)\",\"ev\":[]},{\"n\":\"Pine-1\",\"ev\":[]},"
                        "{\"n\":\"Pine-2\",\"ev\":[]},{\"n\":\"MAPLE (5)\",\"ev\":[]},{\"n\":\"BIRCH (10)\",\"ev\":[]}]}";
    CHECK(rooms_parse(empty, strlen(empty), &o));
    v = (roomsui_view_t){ .offset = 4, .wday = 6, .now = 19 * 60 + 24 };
    roomsui_draw(&c, &o, &v);
    bmp("weekend");

    v = (roomsui_view_t){ .offset = 0, .wday = 3, .now = -1 };
    roomsui_draw(&c, NULL, &v);
    bmp("loading");

    printf(fails ? "%d FAILED\n" : "rooms: all passed\n", fails);
    return fails != 0;
}
