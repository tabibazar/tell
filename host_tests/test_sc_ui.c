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

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BG_PLAIN RGB(0x0E, 0x10, 0x14)

/* How many pixels of colour col there are in the band of rows y0..y1. */
static int count(uint16_t col, int y0, int y1)
{
    int n = 0;
    for (int i = y0 * 480; i < y1 * 480; i++) n += fb[i] == col;
    return n;
}

/* The care features: conversation buttons, the confirm card, captions,
   larger text, high contrast, See and hear, the cards and the friendly clock. */
static void care(canvas_t *c, const uint8_t *lv)
{
    const uint16_t text = RGB(0xF2, 0xF4, 0xF7), said = RGB(0x5A, 0x60, 0x6A), yellow = RGB(0xFF, 0xE6, 0x00);
    int val;

    /* Listening in a conversation: Again, Slower, Done. */
    ui_view_t v = { .mode = UI_LISTENING, .mic_open = true, .now = 9 * 60 + 44, .levels = lv, .nlevels = 40,
                    .listen_ms = 1800, .title = "Your turn", .convo = true };
    ui_draw(c, &v); bmp("listening-convo");
    CHECK(fb[10 * 480 + 240] == RGB(0xE0, 0x1E, 0x1E));                   /* the banner */
    CHECK(ui_hit(&v, 88, 420, &val) == UI_HIT_AGAIN);
    CHECK(ui_hit(&v, 240, 420, &val) == UI_HIT_SLOWER);
    CHECK(ui_hit(&v, 392, 420, &val) == UI_HIT_DONE);
    CHECK(ui_hit(&v, 240, 300, &val) == UI_HIT_NONE);                     /* no accidental stops */
    CHECK(ui_hit(&v, 240, 476, &val) == UI_HIT_NONE);
    v.convo = false;
    CHECK(ui_hit(&v, 240, 420, &val) == UI_HIT_NONE);                     /* without, as before */

    /* The confirm card. */
    v = (ui_view_t){ .mode = UI_LISTENING, .mic_open = true, .now = 9 * 60 + 45, .convo = true,
                     .confirm = "Pay $45 to Green Lawn Care for the October mowing" };
    ui_draw(c, &v); bmp("confirm");
    CHECK(fb[10 * 480 + 240] == RGB(0xE0, 0x1E, 0x1E));
    CHECK(fb[340 * 480 + 130] == RGB(0x2E, 0xA0, 0x4F));                  /* Yes is green */
    CHECK(ui_hit(&v, 130, 380, &val) == UI_HIT_YES);
    CHECK(ui_hit(&v, 350, 380, &val) == UI_HIT_NO);
    CHECK(ui_hit(&v, 240, 200, &val) == UI_HIT_NONE);
    CHECK(ui_hit(&v, 130, 460, &val) == UI_HIT_NONE);
    v.textsize = 1;
    ui_draw(c, &v); bmp("confirm-xl");

    /* Speaking with captions half way, and the Stop button. */
    const char *ans = "You have a design review at two and a call with the board at four. Nothing after five.";
    v = (ui_view_t){ .mode = UI_SPEAKING, .heard = "What's on my calendar this afternoon?", .text = ans,
                     .now = 9 * 60 + 46, .spoken = 40, .convo = true };
    ui_draw(c, &v); bmp("speaking-captions");
    CHECK(count(text, 120, 380) > 100 && count(said, 120, 380) > 100);    /* bright, then dim */
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_DONE);
    CHECK(ui_hit(&v, 240, 300, &val) == UI_HIT_NONE);
    /* The highlight moves, the words do not: the same pixels inked either way. */
    static uint16_t a[480 * 480];
    v.spoken = -1;
    ui_draw(c, &v);
    memcpy(a, fb, sizeof a);
    CHECK(count(said, 120, 380) == 0);
    v.spoken = 40;
    ui_draw(c, &v);
    int moved = 0;
    for (int i = 120 * 480; i < 380 * 480; i++) moved += (a[i] == RGB(0x0E, 0x10, 0x14)) != (fb[i] == RGB(0x0E, 0x10, 0x14));
    CHECK(moved == 0);
    v.note = "asked remote: list_events";
    v.spoken = 0;
    ui_draw(c, &v); bmp("speaking-captions-note");
    CHECK(count(text, 120, 360) == 0);                                    /* nothing said yet: all dim */
    v = (ui_view_t){ .mode = UI_SPEAKING, .heard = "What's on my calendar this afternoon?", .text = ans,
                     .now = 9 * 60 + 46, .spoken = -1, .textsize = 1, .note = "asked remote: list_events" };
    ui_draw(c, &v); bmp("speaking-xl");
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_NONE);                     /* no Stop without a conversation */
    v.text = "Your next payment of two hundred and ten dollars is due on the first of November. "
             "You paid the last one on time, and there is nothing else outstanding on the account today.";
    ui_draw(c, &v); bmp("speaking-xl-long");

    /* High contrast. */
    v = (ui_view_t){ .mode = UI_ANSWER, .heard = "What's on my calendar this afternoon?", .text = ans,
                     .now = 9 * 60 + 46, .contrast = true };
    ui_draw(c, &v); bmp("answer-contrast");
    CHECK(fb[300 * 480 + 470] == 0x0000 && count(yellow, 100, 400) > 100);

    /* Settings, with the new row at the bottom, and See and hear. */
    v = (ui_view_t){ .mode = UI_SETTINGS, .now = 10 * 60, .voice_name = "Roger", .volume = 70, .speed = 1, .model = 0 };
    ui_draw(c, &v); bmp("settings-easy-row");
    CHECK(ui_hit(&v, 240, 456, &val) == UI_HIT_EASY);
    CHECK(ui_hit(&v, 300, 400, &val) == UI_HIT_MODEL && val == 1);
    v = (ui_view_t){ .mode = UI_EASY, .now = 10 * 60, .clarity = true };
    ui_draw(c, &v); bmp("easy");
    CHECK(ui_hit(&v, 30, 30, &val) == UI_HIT_BACK);
    CHECK(ui_hit(&v, 138, 136, &val) == UI_HIT_TEXTSIZE && val == 0);
    CHECK(ui_hit(&v, 342, 136, &val) == UI_HIT_TEXTSIZE && val == 1);
    CHECK(ui_hit(&v, 138, 236, &val) == UI_HIT_CONTRAST && val == 0);
    CHECK(ui_hit(&v, 342, 236, &val) == UI_HIT_CONTRAST && val == 1);
    CHECK(ui_hit(&v, 138, 336, &val) == UI_HIT_CLARITY && val == 0);
    CHECK(ui_hit(&v, 342, 336, &val) == UI_HIT_CLARITY && val == 1);
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_NONE);                     /* the preview */
    v.textsize = 1;
    ui_draw(c, &v); bmp("easy-xl");
    v.contrast = true;
    v.clarity = false;
    ui_draw(c, &v); bmp("easy-xl-contrast");
    CHECK(fb[470 * 480 + 4] == 0x0000 && count(yellow, 64, 480) > 100);

    /* A message from family, a reminder. */
    v = (ui_view_t){ .mode = UI_MESSAGE, .now = 15 * 60 + 2, .title = "Message from Reza",
                     .text = "Hi Mum, I'll come by on Sunday around two. Love you!" };
    ui_draw(c, &v); bmp("message");
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_CLOSE);
    CHECK(ui_hit(&v, 240, 200, &val) == UI_HIT_NONE);
    v.textsize = 1;
    ui_draw(c, &v); bmp("message-xl");
    v = (ui_view_t){ .mode = UI_REMINDER, .now = 18 * 60 + 30,
                     .text = "Trash bins go out tonight; pickup is tomorrow morning." };
    ui_draw(c, &v); bmp("reminder");
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_CLOSE);
    CHECK(count(RGB(0xF5, 0xB0, 0x41), 60, 110) > 50);                   /* the amber bell */
    v.contrast = true;
    ui_draw(c, &v); bmp("reminder-contrast");

    /* The friendly clock. */
    v = (ui_view_t){ .mode = UI_RESTING, .now = 15 * 60 + 12, .wday = 6, .month = 10, .mday = 10 };
    ui_draw(c, &v); bmp("resting-day");
    CHECK(fb[210 * 480 + 240] != 0 && count(RGB(0x5A, 0x60, 0x6A), 360, 430) > 50);

    char part[32], date[32];
    ui_day_words(6, 10, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday afternoon") == 0 && strcmp(date, "October 10") == 0);
    ui_day_words(6, 10, 10, 8 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday morning") == 0);
    ui_day_words(6, 10, 10, 18 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday evening") == 0);
    ui_day_words(6, 10, 10, 23 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday night") == 0);
    ui_day_words(6, 10, 10, 3 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Saturday night") == 0);
    ui_day_words(0, 1, 1, 12 * 60, part, sizeof part, date, sizeof date);
    CHECK(strcmp(part, "Sunday afternoon") == 0 && strcmp(date, "January 1") == 0);
    ui_day_words(6, 0, 10, 15 * 60, part, sizeof part, date, sizeof date);
    CHECK(part[0] == 0 && date[0] == 0);

    /* At rest with the weather: one render per kind of sky, day and night. */
    {
        static const struct { const char *name; int code; bool day; float t; } sky[] = {
            { "resting-weather-sun", 0, true, 21.4f }, { "resting-weather-moon", 0, false, 8.0f },
            { "resting-weather-partly", 2, true, 15.6f }, { "resting-weather-cloud", 3, false, 12.3f },
            { "resting-weather-rain", 63, true, 9.2f }, { "resting-weather-drizzle", 53, true, 10.0f },
            { "resting-weather-snow", 73, true, -4.6f }, { "resting-weather-storm", 95, true, 24.0f },
            { "resting-weather-fog", 45, false, 6.0f },
        };
        for (size_t i = 0; i < sizeof sky / sizeof sky[0]; i++) {
            weather_t w = { .ok = true, .temp = sky[i].t, .feels = sky[i].t - 3, .code = sky[i].code, .day = sky[i].day,
                            .hi = sky[i].t + 4, .lo = sky[i].t - 5, .rain = 40, .t_code = 61, .t_hi = 11.4f, .t_lo = 7.6f };
            v = (ui_view_t){ .mode = UI_RESTING, .now = 21 * 60 + 7, .wday = 2, .month = 10, .mday = 14, .weather = &w };
            ui_draw(c, &v); bmp(sky[i].name);
            CHECK(count(RGB(0xEC, 0xEF, 0xF4), 30, 100) > 300);            /* the time, large */
            CHECK(count(RGB(0xEC, 0xEF, 0xF4), 190, 300) > 300);           /* the temperature and its words */
        }
        weather_t w = { .ok = true, .temp = 12.3f, .feels = 9.4f, .code = 3, .hi = 18.4f, .lo = 11.6f, .rain = 5,
                        .t_code = 63, .t_hi = 11.4f, .t_lo = 7.6f };
        static uint16_t ph[300 * 200];
        for (int i = 0; i < 300 * 200; i++) ph[i] = (uint16_t)(i * 7);
        v = (ui_view_t){ .mode = UI_RESTING, .now = 15 * 60 + 12, .wday = 6, .month = 10, .mday = 10, .weather = &w,
                         .image = ph, .img_w = 300, .img_h = 200, .photo_from = "Reza" };
        ui_draw(c, &v); bmp("resting-photo-weather");
        weather_t none = { 0 };
        v = (ui_view_t){ .mode = UI_RESTING, .now = 15 * 60 + 12, .wday = 6, .month = 10, .mday = 10, .weather = &none };
        ui_draw(c, &v);
        CHECK(fb[210 * 480 + 240] != 0);                                  /* no weather yet: the orb */
    }

    /* Home with the day and the date over the Talk button; none when the date is unknown. */
    v = (ui_view_t){ .mode = UI_HOME, .now = 15 * 60 + 12, .ssid = "Office WiFi", .wday = 6, .month = 10, .mday = 10 };
    ui_draw(c, &v); bmp("home-day");
    CHECK(count(text, 50, 140) > 200);
    CHECK(ui_hit(&v, 240, 272, &val) == UI_HIT_TALK && ui_hit(&v, 438, 438, &val) == UI_HIT_GEAR);
    v.note = "Deepgram key missing";
    ui_draw(c, &v); bmp("home-day-note");
    v.contrast = true;
    ui_draw(c, &v); bmp("home-day-contrast");
    CHECK(count(yellow, 50, 140) > 200);
    v = (ui_view_t){ .mode = UI_HOME, .now = 15 * 60 + 12, .ssid = "Office WiFi", .wday = 6, .month = 0, .mday = 10 };
    ui_draw(c, &v);
    CHECK(count(text, 50, 140) == 0);

    /* A family photo at rest: a 400x300 gradient, scaled to 480x360, centred. */
    static uint16_t photo[400 * 300];
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 400; x++)
            photo[y * 400 + x] = (uint16_t)((31 - x * 31 / 399) << 11 | (20 + y * 43 / 299) << 5 | (x * 31 / 399));
    v = (ui_view_t){ .mode = UI_RESTING, .now = 15 * 60 + 12, .wday = 6, .month = 10, .mday = 10,
                     .image = photo, .img_w = 400, .img_h = 300 };
    ui_draw(c, &v); bmp("resting-photo");
    CHECK(fb[240 * 480 + 240] == photo[150 * 400 + 200]);                 /* the photo, not the orb */
    CHECK(fb[120 * 480 + 0] == photo[50 * 400 + 0]);                      /* edge to edge */
    CHECK(fb[30 * 480 + 240] == 0);                                       /* letterboxed on black */
    CHECK(fb[476 * 480 + 240] == 0 && fb[400 * 480 + 300] != photo[255 * 400 + 250]);  /* the dark band */
    CHECK(count(text, 400, 440) > 50 && count(RGB(0x8A, 0x92, 0x9E), 430, 470) > 50);  /* time, day */
    v.photo_from = "Reza";
    ui_draw(c, &v); bmp("resting-photo-from");
    int dim_right = 0;
    for (int y = 400; y < 430; y++)
        for (int x = 360; x < 480; x++) dim_right += fb[y * 480 + x] == RGB(0x8A, 0x92, 0x9E);
    CHECK(dim_right > 20);                                                /* "from Reza" */
    /* Amber wins over the photo: the orb, exactly as without one. */
    v = (ui_view_t){ .mode = UI_RESTING, .now = 15 * 60 + 12, .amber = true };
    ui_draw(c, &v);
    uint16_t amber_core = fb[210 * 480 + 240];
    v.image = photo; v.img_w = 400; v.img_h = 300; v.photo_from = "Reza";
    ui_draw(c, &v); bmp("resting-photo-amber");
    CHECK(fb[210 * 480 + 240] == amber_core && amber_core == RGB(0xF5, 0xA8, 0x2E));
    CHECK(fb[240 * 480 + 400] == 0);                                      /* no photo behind it */

    /* A photo card: the title on a dark band, the photo, a caption, OK. */
    v = (ui_view_t){ .mode = UI_MESSAGE, .now = 15 * 60 + 14, .title = "A photo from Reza", .photo_from = "Reza",
                     .image = photo, .img_w = 400, .img_h = 300, .text = "The kids at the lake on Sunday" };
    ui_draw(c, &v); bmp("photo-card");
    CHECK(fb[200 * 480 + 240] != C_BG_PLAIN && fb[200 * 480 + 240] != 0);
    CHECK(count(RGB(0x3B, 0x6F, 0xD0), 400, 460) > 5000);               /* the OK */
    CHECK(count(text, 0, 60) > 50 && count(text, 330, 390) > 50);        /* title, caption */
    CHECK(ui_hit(&v, 240, 430, &val) == UI_HIT_CLOSE);
    CHECK(ui_hit(&v, 240, 200, &val) == UI_HIT_NONE);
    v.text = "The kids at the lake on Sunday, and Grandpa's old boat still floating after all these years";
    ui_draw(c, &v); bmp("photo-card-long");
    v.text = NULL;
    ui_draw(c, &v); bmp("photo-card-bare");
    v.contrast = true;
    v.text = "The kids at the lake on Sunday";
    ui_draw(c, &v); bmp("photo-card-contrast");
    CHECK(count(yellow, 0, 60) > 50);
}

int main(void)
{
    system("mkdir -p host_tests/renders/stanclaw");
    canvas_t c;
    canvas_init(&c, fb, 480, 480, 1);

    ui_view_t v = { .mode = UI_HOME, .now = 9 * 60 + 41, .ssid = "Office WiFi" };
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
                     .note = "asked remote: list_events", .now = 9 * 60 + 42, .spoken = -1 };
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

    /* Settings and the voice list. */
    v = (ui_view_t){ .mode = UI_SETTINGS, .now = 10 * 60, .voice_name = "Roger", .volume = 70, .speed = 1, .model = 0 };
    ui_draw(&c, &v); bmp("settings");
    int val;
    CHECK(ui_hit(&v, 30, 30, &val) == UI_HIT_BACK);
    CHECK(ui_hit(&v, 240, 110, &val) == UI_HIT_VOICE);
    CHECK(ui_hit(&v, 240, 218, &val) == UI_HIT_VOLUME && val == 50);
    CHECK(ui_hit(&v, 450, 218, &val) == UI_HIT_VOLUME && val == 100);
    CHECK(ui_hit(&v, 380, 310, &val) == UI_HIT_SPEED && val == 2);
    CHECK(ui_hit(&v, 300, 400, &val) == UI_HIT_MODEL && val == 1);
    v = (ui_view_t){ .mode = UI_VOICES, .now = 10 * 60, .spin = 0.0f, .cur = 0 };
    ui_draw(&c, &v); bmp("voices");                                   /* Roger, in use */
    CHECK(ui_hit(&v, 240, 222, &val) == UI_HIT_CARD && val == 0);
    CHECK(ui_hit(&v, 450, 222, &val) == UI_HIT_CARD && val == 1);   /* the neighbour at the right */
    CHECK(ui_hit(&v, 20, 222, &val) == UI_HIT_NONE);                 /* nothing left of the first */
    v.spin = 4.45f;
    ui_draw(&c, &v); bmp("voices-turning");
    v.spin = 8.0f;
    ui_draw(&c, &v); bmp("voices-hal");
    CHECK(ui_hit(&v, 240, 420, &val) == UI_HIT_USE);
    CHECK(ui_hit(&v, 20, 222, &val) == UI_HIT_CARD && val == 7);
    v.spin = 9.0f;
    ui_draw(&c, &v); bmp("voices-jarvis");
    v = (ui_view_t){ .mode = UI_HOME, .now = 10 * 60 };
    CHECK(ui_hit(&v, 438, 438, &val) == UI_HIT_GEAR && ui_hit(&v, 240, 250, &val) == UI_HIT_TALK);

    care(&c, lv);

    CHECK(ui_hit_talk(240, 250) && ui_hit_talk(240, 272));
    CHECK(!ui_hit_talk(20, 460));

    printf(fails ? "%d FAILED\n" : "sc_ui: all passed\n", fails);
    return fails != 0;
}
