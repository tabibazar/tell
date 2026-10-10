/* stan-claw's screens, drawn to host_tests/renders/stanclaw (one BMP per
   screen) to look at; checks that the LISTENING banner is there whenever
   the mics are open. */
#include "ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint16_t fb[480 * 480];

static void bmp(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "host_tests/renders/stanclaw/%s.bmp", name);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    const int w = 480, h = 480, size = 54 + w * 3 * h;
    unsigned char hd[54] = { 'B', 'M' };
    for (int i = 0; i < 4; i++) {
        hd[2 + i] = (unsigned char)(size >> (8 * i));
        hd[18 + i] = (unsigned char)(w >> (8 * i));
        hd[22 + i] = (unsigned char)(h >> (8 * i));
    }
    hd[10] = 54; hd[14] = 40; hd[26] = 1; hd[28] = 24;
    fwrite(hd, 1, 54, o);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) {
            uint16_t p = fb[y * w + x];
            unsigned char px[3] = { (p & 0x1F) << 3, ((p >> 5) & 0x3F) << 2, (p >> 11) << 3 };
            fwrite(px, 1, 3, o);
        }
    fclose(o);
}

int main(void)
{
    system("mkdir -p host_tests/renders/stanclaw");
    canvas_t c;
    canvas_init(&c, fb, 480, 480, 1);

    ui_view_t v = { .mode = UI_HOME, .now = 9 * 60 + 41 };
    ui_draw(&c, &v); bmp("home");
    uint16_t banner_off = fb[10 * 480 + 240];
    v.note = "Deepgram key missing";
    ui_draw(&c, &v); bmp("home-note");

    /* A voice: two words and a pause, louder in the middle of each. */
    static uint8_t lv[UI_WAVE_BARS];
    for (int i = 0; i < 40; i++) {
        int w = i < 16 ? i : i < 22 ? -1 : i - 22;
        lv[i] = (uint8_t)(w < 0 ? 6 : 40 + (w * 37 % 170) + (w % 3) * 18);
    }
    v = (ui_view_t){ .mode = UI_LISTENING, .mic_open = true, .now = 9 * 60 + 41, .levels = lv, .nlevels = 40, .listen_ms = 2400 };
    ui_draw(&c, &v); bmp("listening");
    CHECK(fb[250 * 480 + 428] != fb[250 * 480 + 2]);         /* the newest bar, at the right, is drawn */
    CHECK(fb[10 * 480 + 240] != banner_off);                 /* the banner is drawn */
    uint16_t banner_on = fb[10 * 480 + 240];

    v = (ui_view_t){ .mode = UI_THINKING, .heard = "What's on my calendar this afternoon?", .now = 9 * 60 + 41 };
    ui_draw(&c, &v); bmp("thinking");

    v = (ui_view_t){ .mode = UI_SPEAKING, .heard = "What's on my calendar this afternoon?",
                     .text = "You have a design review at two and a call with the board at four. Nothing after five.",
                     .note = "asked remote: list_events", .now = 9 * 60 + 42 };
    ui_draw(&c, &v); bmp("speaking");

    v = (ui_view_t){ .mode = UI_ERROR, .title = "Claude key rejected", .text = "Set it again: key claude ...", .now = 9 * 60 + 42 };
    ui_draw(&c, &v); bmp("error");

    v = (ui_view_t){ .mode = UI_AGENT_TEXT, .title = "Build finished", .text = "All 214 tests passed in 3 min 12 s.", .now = 9 * 60 + 43 };
    ui_draw(&c, &v); bmp("agent-text");

    static uint16_t img[200 * 120];
    for (int i = 0; i < 200 * 120; i++) img[i] = (uint16_t)((i % 200) * 31 / 200 << 11 | (i / 200) * 63 / 120 << 5);
    v = (ui_view_t){ .mode = UI_AGENT_IMAGE, .image = img, .img_w = 200, .img_h = 120, .mic_open = true, .now = -1 };
    ui_draw(&c, &v); bmp("agent-image-listening");
    CHECK(fb[10 * 480 + 240] == banner_on);                  /* even over an agent's picture */

    v = (ui_view_t){ .mode = UI_RESTING, .now = 14 * 60 + 5 };
    ui_draw(&c, &v); bmp("resting");
    CHECK(fb[210 * 480 + 240] != 0 && fb[20 * 480 + 20] == 0);      /* an orb on black */
    uint16_t blue = fb[210 * 480 + 240];
    v.amber = true;
    ui_draw(&c, &v); bmp("resting-amber");
    CHECK(fb[210 * 480 + 240] != blue);
    v = (ui_view_t){ .mode = UI_RESTING, .now = 14 * 60 + 5, .mic_open = true };
    ui_draw(&c, &v);
    CHECK(fb[10 * 480 + 240] == banner_on);                  /* the banner, even resting */

    CHECK(ui_hit_talk(240, 250));
    CHECK(!ui_hit_talk(20, 460));

    printf(fails ? "%d FAILED\n" : "sc_ui: all passed\n", fails);
    return fails != 0;
}
