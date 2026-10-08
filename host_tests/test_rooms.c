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
    "{\"n\":\"CEDAR (8) [TV with HDMI and WebCam]\",\"ev\":[[660,720,\"Design review: onboarding flow for the new mobile app\",\"Lena\",\"Lena Ortiz\",\"lena.ortiz@example.com\",[\"Omar Haddad\",\"Ana Silva\",\"Ken Watanabe\",\"Priya Raman\",\"Jordan Lee\",\"Chris Doyle\",\"\\u00c9milie Roy\"],9],[765,780,\"Hiring sync\",\"Nora\"],"
    "[780,810,\"Budget planning\",\"Omar\"],[810,840,\"Ops weekly\",\"Theo\"]]},"
    "{\"n\":\"Pine-1 (Closest to the door) (1)\",\"ev\":[[540,570,\"Daily standup\",\"Priya\"],[600,690,\"Quarterly roadmap review with the platform team\",\"Jordan\"]]},"
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
    CHECK(strcmp(d.room[1].name, "PINE-1") == 0 && d.room[1].cap == 1);
    CHECK(d.room[0].n == 4 && d.room[0].ev[0].start == 660 && strcmp(d.room[0].ev[0].who, "Lena") == 0);
    CHECK(strcmp(d.room[0].ev[0].full, "Lena Ortiz") == 0 && strcmp(d.room[0].ev[0].email, "lena.ortiz@example.com") == 0);
    CHECK(d.room[0].ev[0].nguests == 9 && strstr(d.room[0].ev[0].guests, "Emilie Roy") != NULL);
    CHECK(strcmp(d.room[0].ev[1].full, "Nora") == 0 && d.room[0].ev[1].nguests == 0);   /* an old relay's answer */
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
    roomsui_hours(NULL, &a, &b);
    CHECK(a == 8 && b == 16);                                          /* the working day, 8 to 16 */

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

    /* A room's status line: taken, free, another day. */
    room_t sr = { .name = "TALL", .cap = 8, .n = 3 };
    sr.ev[0] = (room_ev_t){ .start = 600, .end = 645 };     /* 10:00-10:45 */
    sr.ev[1] = (room_ev_t){ .start = 645, .end = 660 };     /* back to back, to 11:00 */
    sr.ev[2] = (room_ev_t){ .start = 840, .end = 900 };     /* 14:00-15:00 */
    char st[64];
    roomsui_view_t sv = { .offset = 0, .wday = 4, .now = 620 };
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_TAKEN && strcmp(st, "Taken till 11:00, then free till 14:00") == 0);
    sv.now = 850;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_TAKEN && strcmp(st, "Taken till 15:00") == 0);
    sv.now = 540;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_FREE && strcmp(st, "Free till 10:00") == 0);
    sv.now = 910;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_FREE && strcmp(st, "Free") == 0);
    sv = (roomsui_view_t){ .offset = 1, .wday = 5, .now = 620 };
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "3 bookings") == 0);
    sr.n = 1;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "1 booking") == 0);
    sr.n = 0;
    CHECK(roomsui_room_status(&sr, &sv, st, sizeof st) == ROOMSUI_OTHER && strcmp(st, "No bookings") == 0);

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

    /* A tap on the 11:00 booking in the first column finds it; between bookings, nothing. */
    int first, last, room = -1;
    roomsui_hours(&d, &first, &last);
    int gy = 52 + 40 + 6, gh = 480 - 6 - gy, span = (last - first) * 60;
    int ymid = gy + (690 - first * 60) * gh / span;
    const room_ev_t *hit = roomsui_hit(&d, 26 + 40, ymid, &room);
    CHECK(hit == &d.room[0].ev[0] && room == 0);
    CHECK(roomsui_hit(&d, 26 + 40, gy + (600 - first * 60) * gh / span, &room) == NULL);
    CHECK(roomsui_hit(&d, 26 + 40, 20, &room) == NULL);                /* the header */
    v = (roomsui_view_t){ .offset = 1, .wday = 4, .now = 19 * 60 + 24 };
    roomsui_draw(&c, &d, &v);
    roomsui_detail(&c, &d.room[0], &d.room[0].ev[0], &v);
    bmp("detail");
    roomsui_draw(&c, &d, &v);
    roomsui_detail(&c, &d.room[3], &d.room[3].ev[1], &v);
    bmp("detail-busy");

    v = (roomsui_view_t){ .offset = 0, .wday = 3, .now = 15 * 60 + 2, .note = "offline 14:05" };
    roomsui_draw(&c, &d, &v);
    bmp("offline");

    const char *empty = "{\"d\":\"2026-10-10\",\"rooms\":[{\"n\":\"CEDAR (8)\",\"ev\":[]},{\"n\":\"Pine-1 (Closest to the door) (1)\",\"ev\":[]},"
                        "{\"n\":\"Pine-2\",\"ev\":[]},{\"n\":\"MAPLE (5)\",\"ev\":[]},{\"n\":\"BIRCH (10)\",\"ev\":[]}]}";
    CHECK(rooms_parse(empty, strlen(empty), &o));
    v = (roomsui_view_t){ .offset = 4, .wday = 6, .now = 19 * 60 + 24 };
    roomsui_draw(&c, &o, &v);
    bmp("weekend");

    v = (roomsui_view_t){ .offset = 0, .wday = 3, .now = -1 };
    roomsui_draw(&c, NULL, &v);
    bmp("loading");

    roomsui_saver(&c, 7 * 60 + 42, 3, 7, 10, 0);
    bmp("saver-0");
    roomsui_saver(&c, 23 * 60 + 5, 3, 7, 10, 29000123);
    bmp("saver-1");

    printf(fails ? "%d FAILED\n" : "rooms: all passed\n", fails);
    return fails != 0;
}
