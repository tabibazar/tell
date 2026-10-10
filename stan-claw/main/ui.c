#include "ui.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

#include "aafont.h"

extern const aafont_t aafont_rooms_head;    /* Inter SemiBold 30 */
extern const aafont_t aafont_rooms_title;   /* Inter SemiBold 17 */
extern const aafont_t aafont_rooms_small;   /* Inter Medium 15 */
extern const aafont_t aafont_sc_answer_xl;  /* Inter SemiBold 40: answers, extra large */

#define W 480
#define H 480
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
/* The ground and the two text colours change with high contrast (set at the
   top of ui_draw): pure black, yellow text, white for what is secondary. */
static uint16_t C_BG   = RGB(0x0E, 0x10, 0x14);
static uint16_t C_TEXT = RGB(0xF2, 0xF4, 0xF7);
static uint16_t C_DIM  = RGB(0x8A, 0x92, 0x9E);
static uint16_t C_SAID = RGB(0x5A, 0x60, 0x6A);     /* captions: the words still to come */
static bool contrast;
#define C_AMBER  RGB(0xF5, 0xB0, 0x41)
#define C_BLUE   RGB(0x3B, 0x6F, 0xD0)
#define C_RED    RGB(0xD9, 0x3A, 0x3A)
#define C_BANNER RGB(0xE0, 0x1E, 0x1E)
#define C_GREEN  RGB(0x2E, 0xA0, 0x4F)
#define C_CLOCK  RGB(0x5A, 0x60, 0x6A)          /* the resting time and day */

#define TALK_X 240
#define TALK_Y 250
#define TALK_R 100

bool ui_hit_talk(int x, int y)
{
    int dx = x - TALK_X, dy = y - TALK_Y;
    return dx * dx + dy * dy <= (TALK_R + 20) * (TALK_R + 20);
}

/* Greedy word wrap of `s` in face `f` into lines of `maxw`, drawn from
   (x, y) every `pitch` px, at most `max` lines (the last one "..."-cut).
   With captions (`hl` >= 0) the words starting before offset `hl` of `s`
   are drawn in `col` and the rest in `dim`; the wrap is the same either way,
   so the text never moves as the highlight walks through it. Returns the y
   below the last line. */
static int para_hl(canvas_t *c, const aafont_t *f, const char *s, int x, int y, int maxw, int pitch, int max,
                   uint16_t col, int hl, uint16_t dim)
{
    const char *base = s;
    char line[160];
    char trunc[164];
    int lines = 0;
    while (s && *s && lines < max) {
        while (*s == ' ') s++;
        int best = 0, i = 0;
        while (s[i] && i < (int)sizeof line - 1) {
            int j = i;
            while (s[j] && s[j] != ' ' && j < (int)sizeof line - 1) j++;
            memcpy(line, s, j);
            line[j] = 0;
            if (aafont_width(f, line) > maxw) break;
            best = j;
            if (!s[j]) break;
            i = j + 1;
        }
        if (best == 0) {                       /* one word wider than the line */
            best = 1;
            while (s[best] && s[best] != ' ' && best < (int)sizeof line - 1) {
                memcpy(line, s, best + 1);
                line[best + 1] = 0;
                if (aafont_width(f, line) > maxw) break;
                best++;
            }
        }
        memcpy(line, s, best);
        line[best] = 0;
        s += best;
        const char *out = line;
        if (lines == max - 1 && *s) {          /* the last allowed line, with more to come */
            size_t l = strlen(line);
            while (l > 0) {
                snprintf(trunc, sizeof trunc, "%.*s...", (int)l, line);
                if (aafont_width(f, trunc) <= maxw) { out = trunc; break; }
                l--;
            }
        }
        if (hl < 0) {
            aafont_draw(c, f, x, y, out, col, AAFONT_LEFT);
        } else {
            /* Where the dim part begins: the first word starting at or past hl. */
            int off = (int)(s - best - base), k = 0, n = (int)strlen(out);
            while (k < n && !(out[k] != ' ' && (k == 0 || out[k - 1] == ' ') && off + k >= hl)) k++;
            /* The whole line dim, then the bright words over it from the same
               x: the same first glyph, so the same pen, so every glyph lands
               where it was. */
            if (k < n) aafont_draw(c, f, x, y, out, dim, AAFONT_LEFT);
            if (k > 0) {
                char head[164];
                snprintf(head, sizeof head, "%.*s", k, out);
                aafont_draw(c, f, x, y, head, col, AAFONT_LEFT);
            }
        }
        y += pitch;
        lines++;
    }
    return y;
}

static int para(canvas_t *c, const aafont_t *f, const char *s, int x, int y, int maxw, int pitch, int max, uint16_t col)
{
    return para_hl(c, f, s, x, y, maxw, pitch, max, col, -1, col);
}

/* The answers' face at the chosen text size, its line pitch, and how many of
   its lines fit from y down to `bottom` (the last line's descenders in). */
static const aafont_t *answer_face(const ui_view_t *v, int *pitch)
{
    *pitch = v->textsize ? 54 : 42;
    return v->textsize ? &aafont_sc_answer_xl : &aafont_rooms_head;
}

static int lines_fit(const aafont_t *f, int pitch, int y, int bottom)
{
    int n = (bottom - y - f->cap - f->descent) / pitch + 1;
    return n < 1 ? 1 : n;
}

/* A WiFi mark: three arcs over a dot, drawn as rings cut to their top
   quarter, centred on (cx, base). */
static void wifi_mark(canvas_t *c, int cx, int base, uint16_t col)
{
    canvas_disc(c, cx, base - 1, 2, col);
    for (int r = 5; r <= 13; r += 4)
        for (int dx = -r; dx <= r; dx++)
            for (int dy = -r; dy <= 0; dy++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= r * r && d2 > (r - 2) * (r - 2) && -dy * 10 >= (dx < 0 ? -dx : dx) * 8)
                    canvas_fill_rect(c, cx + dx, base - 1 + dy, 1, 1, col);
            }
}

static void header(canvas_t *c, const ui_view_t *v)
{
    aafont_draw(c, &aafont_rooms_small, 16, 18, "stan-claw", C_DIM, AAFONT_LEFT);
    /* The network, top centre: the mark and the name as one, centred. */
    const aafont_t *f = &aafont_rooms_small;
    bool on = v->ssid && v->ssid[0];
    char name[34];
    snprintf(name, sizeof name, "%s", on ? v->ssid : "no WiFi");
    while (name[0] && aafont_width(f, name) > 220) name[strlen(name) - 1] = 0;
    int w = 16 + 8 + aafont_width(f, name), x = (W - w) / 2;
    uint16_t col = on ? C_DIM : C_AMBER;
    wifi_mark(c, x + 8, 18 + f->cap, col);
    aafont_draw(c, f, x + 24, 18, name, col, AAFONT_LEFT);
    if (v->now >= 0) {
        char t[24];
        snprintf(t, sizeof t, "%d:%02d", v->now / 60 % 24, v->now % 60);
        aafont_draw(c, &aafont_rooms_small, W - 16, 18, t, C_DIM, AAFONT_RIGHT);
    }
}

static void note(canvas_t *c, const char *n)
{
    if (n && n[0]) aafont_draw(c, &aafont_rooms_small, W / 2, H - 34, n, C_AMBER, AAFONT_CENTRE);
}

static void talk_button(canvas_t *c, uint16_t col, const char *label, const char *hint)
{
    canvas_disc(c, TALK_X, TALK_Y, TALK_R, col);
    aafont_draw(c, &aafont_rooms_head, TALK_X, TALK_Y - aafont_rooms_head.cap / 2, label, C_TEXT, AAFONT_CENTRE);
    if (hint) aafont_draw(c, &aafont_rooms_small, W / 2, TALK_Y + TALK_R + 24, hint, C_DIM, AAFONT_CENTRE);
}

static uint16_t mix(uint16_t a, uint16_t b, float t);

/* ---- settings ---- */

#define GEAR_X 438
#define GEAR_Y 438
#define C_LINE RGB(0x24, 0x28, 0x30)
#define C_KNOB RGB(0xF2, 0xF4, 0xF7)
#define C_BTN  RGB(0x22, 0x26, 0x2E)

static void gear(canvas_t *c, int cx, int cy)
{
    for (int i = 0; i < 8; i++) {
        static const int dx[8] = { 0, 11, 15, 11, 0, -11, -15, -11 }, dy[8] = { -15, -11, 0, 11, 15, 11, 0, -11 };
        canvas_disc(c, cx + dx[i], cy + dy[i], 4, C_DIM);
    }
    canvas_disc(c, cx, cy, 12, C_DIM);
    canvas_disc(c, cx, cy, 5, C_BG);
}

/* A rectangle with corners of radius r. */
static void rrect(canvas_t *c, int x, int y, int w, int h, int r, uint16_t col)
{
    canvas_fill_rect(c, x + r, y, w - 2 * r, h, col);
    canvas_fill_rect(c, x, y + r, w, h - 2 * r, col);
    canvas_disc(c, x + r, y + r, r, col);
    canvas_disc(c, x + w - 1 - r, y + r, r, col);
    canvas_disc(c, x + r, y + h - 1 - r, r, col);
    canvas_disc(c, x + w - 1 - r, y + h - 1 - r, r, col);
}

/* A button's plate in bg; on high contrast with a 2 px yellow edge, so
   it stands out from the black. */
static void plate(canvas_t *c, int x, int y, int w, int h, int r, uint16_t bg)
{
    if (contrast) {
        rrect(c, x, y, w, h, r, C_TEXT);
        rrect(c, x + 2, y + 2, w - 4, h - 4, r - 2 > 1 ? r - 2 : 1, bg);
    } else {
        rrect(c, x, y, w, h, r, bg);
    }
}

/* A rounded button with centred text in face f; `on` lights it. */
static void button_f(canvas_t *c, const aafont_t *f, int x, int y, int w, int h, const char *t, bool on)
{
    plate(c, x, y, w, h, 3, on ? C_BLUE : C_BTN);
    aafont_draw(c, f, x + w / 2, y + (h - f->cap) / 2, t, on ? C_TEXT : C_DIM, AAFONT_CENTRE);
}

static void button(canvas_t *c, int x, int y, int w, int h, const char *t, bool on)
{
    button_f(c, &aafont_rooms_title, x, y, w, h, t, on);
}

/* A big button for a finger that is not so sure: a word in the large face
   and, under it, what it does in small. */
static void big_button(canvas_t *c, int x, int y, int w, int h, const char *t, const char *sub, uint16_t bg)
{
    const aafont_t *f = &aafont_rooms_head, *sf = &aafont_rooms_small;
    plate(c, x, y, w, h, 10, bg);
    int th = sub ? f->cap + 12 + sf->cap : f->cap;
    int ty = y + (h - th) / 2;
    bool plain = bg == C_BTN;
    aafont_draw(c, f, x + w / 2, ty, t, plain ? C_TEXT : RGB(0xFF, 0xFF, 0xFF), AAFONT_CENTRE);
    if (sub) aafont_draw(c, sf, x + w / 2, ty + f->cap + 12, sub, plain ? C_DIM : mix(RGB(0xFF, 0xFF, 0xFF), bg, 0.25f),
                         AAFONT_CENTRE);
}

/* A rectangle a hit falls in, with `m` px to spare all round. */
static bool in_rect(int x, int y, int rx, int ry, int rw, int rh, int m)
{
    return x >= rx - m && x < rx + rw + m && y >= ry - m && y < ry + rh + m;
}

/* A back chevron and the page's title, on a plain bar. */
#define TOP_H 64
static void topbar(canvas_t *c, const char *title)
{
    canvas_fill_rect(c, 0, 0, W, TOP_H, C_BG);
    for (int i = 0; i < 10; i++) {                       /* the chevron: two strokes */
        canvas_fill_rect(c, 21 + i, 32 - i - 1, 3, 2, C_TEXT);
        canvas_fill_rect(c, 21 + i, 32 + i - 1, 3, 2, C_TEXT);
    }
    aafont_draw(c, &aafont_rooms_head, 52, 32 - aafont_rooms_head.cap / 2, title, C_TEXT, AAFONT_LEFT);
    canvas_fill_rect(c, 0, TOP_H - 1, W, 1, C_LINE);
}

/* Layout of the settings page, shared by drawing and hits. */
#define ROW_VOICE_Y   (TOP_H)
#define ROW_VOICE_H   86
#define SLIDER_X0     40
#define SLIDER_X1     440
#define SLIDER_Y      218
#define SPEED_Y       284
#define MODEL_Y       368
#define SEG_H         44
#define EASY_ROW_Y    432           /* "Easier to see and hear", to the bottom */

static void label(canvas_t *c, int y, const char *t)
{
    aafont_draw(c, &aafont_rooms_small, 40, y, t, C_DIM, AAFONT_LEFT);
}

/* A chevron pointing right, its tip at (x + 8, y). */
static void chevron(canvas_t *c, int x, int y, uint16_t col)
{
    for (int i = 0; i < 8; i++) {
        canvas_fill_rect(c, x + i, y - 8 + i, 2, 2, col);
        canvas_fill_rect(c, x + i, y + 8 - i, 2, 2, col);
    }
}

static void settings(canvas_t *c, const ui_view_t *v)
{
    topbar(c, "Settings");
    /* Voice: name, a hint, a chevron to the list. */
    label(c, ROW_VOICE_Y + 16, "VOICE");
    aafont_draw(c, &aafont_inter_sub, 40, ROW_VOICE_Y + 40, v->voice_name && v->voice_name[0] ? v->voice_name : "not chosen",
                C_TEXT, AAFONT_LEFT);
    chevron(c, 432, ROW_VOICE_Y + 44, C_DIM);
    canvas_fill_rect(c, 0, ROW_VOICE_Y + ROW_VOICE_H, W, 1, C_LINE);

    /* Volume: a track, the filled part, a knob. */
    char t[16];
    label(c, SLIDER_Y - 40, "VOLUME");
    snprintf(t, sizeof t, "%d", v->volume);
    aafont_draw(c, &aafont_rooms_small, SLIDER_X1, SLIDER_Y - 40, t, C_DIM, AAFONT_RIGHT);
    int kx = SLIDER_X0 + v->volume * (SLIDER_X1 - SLIDER_X0) / 100;
    canvas_fill_rect(c, SLIDER_X0, SLIDER_Y - 2, SLIDER_X1 - SLIDER_X0, 4, C_BTN);
    canvas_fill_rect(c, SLIDER_X0, SLIDER_Y - 2, kx - SLIDER_X0, 4, C_BLUE);
    canvas_disc(c, kx, SLIDER_Y, 12, C_KNOB);

    label(c, SPEED_Y - 26, "SPEAKING SPEED");
    static const char *const sp[3] = { "Slower", "Normal", "Faster" };
    for (int i = 0; i < 3; i++) button(c, 40 + i * 136, SPEED_Y, 128, SEG_H, sp[i], v->speed == i);

    label(c, MODEL_Y - 26, "CLAUDE");
    button(c, 40, MODEL_Y, 196, SEG_H, "Sonnet 5 - smarter", v->model == 0);
    button(c, 244, MODEL_Y, 196, SEG_H, "Haiku 4.5 - faster", v->model == 1);

    /* To the See and hear page. */
    canvas_fill_rect(c, 0, EASY_ROW_Y, W, 1, C_LINE);
    int ry = EASY_ROW_Y + (H - EASY_ROW_Y - aafont_inter_sub.cap) / 2;
    aafont_draw(c, &aafont_inter_sub, 40, ry, "Easier to see and hear", C_TEXT, AAFONT_LEFT);
    chevron(c, 432, ry + aafont_inter_sub.cap / 2, C_DIM);
}

/* ---- See and hear: text size, contrast, clarity ---- */

#define EASY_Y0    104          /* the first pair of buttons; each group EASY_STEP below the last */
#define EASY_STEP  100
#define EASY_BH    64
#define EASY_PREVIEW_Y 388

static void easy(canvas_t *c, const ui_view_t *v)
{
    topbar(c, "See and hear");
    static const char *const names[3] = { "TEXT SIZE", "CONTRAST", "CLARITY FOR HEARING" };
    static const char *const opts[3][2] = { { "Large", "Extra large" }, { "Normal", "High" }, { "Off", "On" } };
    int lit[3] = { v->textsize ? 1 : 0, v->contrast, v->clarity };
    for (int g = 0; g < 3; g++) {
        int y = EASY_Y0 + g * EASY_STEP;
        label(c, y - 22, names[g]);
        for (int i = 0; i < 2; i++) button_f(c, &aafont_rooms_head, 40 + i * 204, y, 196, EASY_BH, opts[g][i], lit[g] == i);
    }
    /* What an answer will look like, in its own face and size. */
    int pitch;
    const aafont_t *f = answer_face(v, &pitch);
    para(c, f, "This is how answers look.", 40, EASY_PREVIEW_Y, W - 80, pitch, 2, C_TEXT);
}

static void check(canvas_t *c, int x, int y, uint16_t col)
{
    for (int i = 0; i < 6; i++) canvas_fill_rect(c, x + i, y + i, 3, 3, col);          /* the short stroke */
    for (int i = 0; i < 12; i++) canvas_fill_rect(c, x + 5 + i, y + 5 - i, 3, 3, col); /* the long one */
}

/* ---- the voice wheel ---- */

#define WHEEL_Y   222           /* cards' centre line */
#define CARD_W    250
#define CARD_H    230
#define CARD_STEP 205           /* px between neighbouring cards' centres */
#define USE_Y     402

static uint16_t card_colour(int i)
{
    const voice_t *v = &VOICES[i];
    if (v->fx == FX_HAL) return RGB(0xE0, 0x2A, 0x2A);
    if (v->fx == FX_JARVIS) return RGB(0x3C, 0xD6, 0xF0);
    return strncmp(v->tags, "female", 6) == 0 ? RGB(0xE8, 0x7A, 0xB4) : RGB(0x5A, 0x8E, 0xF0);
}

/* The emblem at the top of a card: a person's initial in a coloured circle;
   Hal a red eye that glows; Jarvis a cyan ring. */
static void emblem(canvas_t *c, int i, int cx, int cy, int r, float fade)
{
    uint16_t col = mix(card_colour(i), C_BTN, fade);
    if (VOICES[i].fx == FX_HAL) {
        for (int k = r; k > 0; k -= 2) canvas_disc(c, cx, cy, k, mix(col, RGB(0x10, 0x10, 0x12), (float)k / r * 0.9f));
        canvas_disc(c, cx, cy, r / 4, mix(RGB(0xFF, 0xE0, 0x60), C_BTN, fade));
    } else if (VOICES[i].fx == FX_JARVIS) {
        canvas_disc(c, cx, cy, r, col);
        canvas_disc(c, cx, cy, r - r / 4, C_BTN);
        canvas_disc(c, cx, cy, r / 3, col);
    } else {
        canvas_disc(c, cx, cy, r, col);
        char ini[2] = { VOICES[i].name[0], 0 };
        aafont_draw(c, &aafont_rooms_head, cx, cy - aafont_rooms_head.cap / 2, ini, mix(C_TEXT, C_BTN, fade), AAFONT_CENTRE);
    }
}

/* One card at centre x, d cards from the middle (0 = the chosen one). */
static void card(canvas_t *c, int i, int cx, float d, bool in_use)
{
    float ad = d < 0 ? -d : d;
    float scale = 1.0f - 0.24f * (ad > 1.5f ? 1.5f : ad);
    float fade = ad > 1.0f ? 0.65f : ad * 0.65f;
    int w = (int)(CARD_W * scale), h = (int)(CARD_H * scale);
    int x = cx - w / 2, y = WHEEL_Y - h / 2;
    uint16_t bg = mix(RGB(0x22, 0x27, 0x31), C_BG, fade * 0.5f);
    canvas_fill_rect(c, x + 8, y, w - 16, h, bg);
    canvas_fill_rect(c, x, y + 8, w, h - 16, bg);
    canvas_disc(c, x + 8, y + 8, 8, bg);
    canvas_disc(c, x + w - 9, y + 8, 8, bg);
    canvas_disc(c, x + 8, y + h - 9, 8, bg);
    canvas_disc(c, x + w - 9, y + h - 9, 8, bg);
    if (ad < 0.5f) {                                        /* the chosen card: a thin coloured edge */
        uint16_t e = card_colour(i);
        canvas_fill_rect(c, x + 8, y, w - 16, 2, e);
        canvas_fill_rect(c, x + 8, y + h - 2, w - 16, 2, e);
    }
    emblem(c, i, cx, y + (int)(58 * scale), (int)(34 * scale), fade);
    const aafont_t *nf = ad < 0.5f ? &aafont_rooms_head : &aafont_rooms_title;
    aafont_draw(c, nf, cx, y + (int)(112 * scale), VOICES[i].name, mix(C_TEXT, C_BG, fade), AAFONT_CENTRE);
    if (ad < 0.8f) {
        char t[64];
        snprintf(t, sizeof t, "%s", VOICES[i].desc);
        while (t[0] && aafont_width(&aafont_rooms_small, t) > w - 24) t[strlen(t) - 1] = 0;
        aafont_draw(c, &aafont_rooms_small, cx, y + (int)(156 * scale), t, mix(C_DIM, C_BG, fade), AAFONT_CENTRE);
        snprintf(t, sizeof t, "%s", VOICES[i].tags);
        aafont_draw(c, &aafont_rooms_small, cx, y + (int)(182 * scale), t, mix(card_colour(i), C_BG, fade + 0.2f), AAFONT_CENTRE);
    }
    if (in_use) check(c, x + w - 34, y + 16, card_colour(i));
}


static void voices_page(canvas_t *c, const ui_view_t *v)
{
    /* Farthest cards first, so nearer ones sit on top. */
    int order[VOICES_N];
    for (int i = 0; i < VOICES_N; i++) order[i] = i;
    for (int a = 0; a < VOICES_N; a++)
        for (int b = a + 1; b < VOICES_N; b++) {
            float da = fabsf(order[a] - v->spin), db = fabsf(order[b] - v->spin);
            if (db > da) { int t = order[a]; order[a] = order[b]; order[b] = t; }
        }
    for (int k = 0; k < VOICES_N; k++) {
        int i = order[k];
        float d = i - v->spin;
        int cx = W / 2 + (int)lroundf(d * CARD_STEP);
        if (cx < -CARD_W / 2 || cx > W + CARD_W / 2) continue;
        card(c, i, cx, d, i == v->cur);
    }
    topbar(c, "Voice");
    /* A dot for each voice, the middle one lit. */
    int mid = (int)lroundf(v->spin);
    for (int i = 0; i < VOICES_N; i++)
        canvas_disc(c, W / 2 + (i - (VOICES_N - 1) / 2.0f) * 16, 360, i == mid ? 4 : 3, i == mid ? card_colour(i) : C_BTN);
    float off = v->spin - mid;
    bool settled = off > -0.02f && off < 0.02f;
    if (mid != v->cur && settled) {
        char t[40];
        snprintf(t, sizeof t, "Use %s", VOICES[mid].name);
        button(c, 70, USE_Y, W - 140, 50, t, true);
    } else if (mid == v->cur) {
        aafont_draw(c, &aafont_rooms_small, W / 2, USE_Y + 18, "in use  -  swipe for others", C_DIM, AAFONT_CENTRE);
    }
}

/* In a conversation, three buttons along the bottom of the listening page;
   on the speaking page, one to stop. */
#define CONVO_Y   380
#define CONVO_W   140
#define CONVO_H   80
#define CONVO_X0  ((W - 3 * CONVO_W - 2 * 12) / 2)
#define STOP_X    120
#define STOP_Y    400
#define STOP_W    240
#define STOP_H    60

/* The Yes / No card. */
#define YN_Y      330
#define YN_W      200
#define YN_H      100
#define YES_X     30
#define NO_X      250
#define C_NO      RGB(0x4A, 0x50, 0x5A)

/* The OK on a message or reminder card. */
#define OK_X      160
#define OK_Y      400
#define OK_W      160
#define OK_H      60

int ui_slider_value(int x)
{
    int vol = (x - SLIDER_X0) * 100 / (SLIDER_X1 - SLIDER_X0);
    return vol < 0 ? 0 : vol > 100 ? 100 : vol;
}

ui_hit_t ui_hit(const ui_view_t *v, int x, int y, int *value)
{
    *value = 0;
    if (v->mode == UI_HOME) {
        int dx = x - GEAR_X, dy = y - GEAR_Y;
        if (dx * dx + dy * dy <= 36 * 36) return UI_HIT_GEAR;
        return ui_hit_talk(x, y) ? UI_HIT_TALK : UI_HIT_NONE;
    }
    if (v->mode == UI_LISTENING && v->confirm && v->confirm[0]) {
        if (in_rect(x, y, YES_X, YN_Y, YN_W, YN_H, 10)) return UI_HIT_YES;
        if (in_rect(x, y, NO_X, YN_Y, YN_W, YN_H, 10)) return UI_HIT_NO;
        return UI_HIT_NONE;
    }
    if (v->mode == UI_LISTENING && v->convo) {
        static const ui_hit_t h[3] = { UI_HIT_AGAIN, UI_HIT_SLOWER, UI_HIT_DONE };
        for (int i = 0; i < 3; i++)
            if (in_rect(x, y, CONVO_X0 + i * (CONVO_W + 12), CONVO_Y, CONVO_W, CONVO_H, 6)) return h[i];
        return UI_HIT_NONE;
    }
    if (v->mode == UI_SPEAKING && v->convo)
        return in_rect(x, y, STOP_X, STOP_Y, STOP_W, STOP_H, 12) ? UI_HIT_DONE : UI_HIT_NONE;
    if (v->mode == UI_MESSAGE || v->mode == UI_REMINDER)
        return in_rect(x, y, OK_X, OK_Y, OK_W, OK_H, 12) ? UI_HIT_CLOSE : UI_HIT_NONE;
    if (v->mode != UI_SETTINGS && v->mode != UI_VOICES && v->mode != UI_EASY) return UI_HIT_NONE;
    if (y < TOP_H && x < 160) return UI_HIT_BACK;
    if (v->mode == UI_EASY) {
        static const ui_hit_t h[3] = { UI_HIT_TEXTSIZE, UI_HIT_CONTRAST, UI_HIT_CLARITY };
        for (int g = 0; g < 3; g++) {
            int by = EASY_Y0 + g * EASY_STEP;
            if (in_rect(x, y, 40, by, 400, EASY_BH, 8)) {
                *value = x < 240 ? 0 : 1;
                return h[g];
            }
        }
        return UI_HIT_NONE;
    }
    if (v->mode == UI_SETTINGS) {
        if (y >= ROW_VOICE_Y && y < ROW_VOICE_Y + ROW_VOICE_H) return UI_HIT_VOICE;
        if (y >= SLIDER_Y - 30 && y <= SLIDER_Y + 30) {
            int vol = (x - SLIDER_X0) * 100 / (SLIDER_X1 - SLIDER_X0);
            *value = vol < 0 ? 0 : vol > 100 ? 100 : vol;
            return UI_HIT_VOLUME;
        }
        if (y >= SPEED_Y - 6 && y < SPEED_Y + SEG_H + 6 && x >= 40 && x < 440) {
            *value = (x - 40) / 136;
            if (*value > 2) *value = 2;
            return UI_HIT_SPEED;
        }
        if (y >= MODEL_Y - 6 && y < MODEL_Y + SEG_H + 6 && x >= 40 && x < 440) {
            *value = x < 240 ? 0 : 1;
            return UI_HIT_MODEL;
        }
        if (y >= EASY_ROW_Y) return UI_HIT_EASY;
        return UI_HIT_NONE;
    }
    if (y >= USE_Y - 10 && y < USE_Y + 64) return UI_HIT_USE;
    if (y < WHEEL_Y - CARD_H / 2 || y > WHEEL_Y + CARD_H / 2) return UI_HIT_NONE;
    int mid = (int)lroundf(v->spin);
    int i = x < W / 2 - CARD_W / 2 ? mid - 1 : x > W / 2 + CARD_W / 2 ? mid + 1 : mid;
    if (i < 0 || i >= VOICES_N) return UI_HIT_NONE;
    *value = i;
    return UI_HIT_CARD;
}

/* The microphones are open: a red bar across the top, the dot and the word
   centred together as one. */
static void banner(canvas_t *c)
{
    const aafont_t *f = &aafont_rooms_title;
    const char *t = "LISTENING";
    const int r = 5, gap = 10;
    int w = 2 * r + gap + aafont_width(f, t);
    int x = (W - w) / 2;
    canvas_fill_rect(c, 0, 0, W, 44, C_BANNER);
    canvas_disc(c, x + r, 22, r, C_TEXT);
    aafont_draw(c, f, x + 2 * r + gap, 22 - f->cap / 2, t, C_TEXT, AAFONT_LEFT);
}

/* A colour part way from a to b, t in 0..1 (each channel, gamma ignored:
   for a fade of bars it reads well enough). */
static uint16_t mix(uint16_t a, uint16_t b, float t)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t));
}

/* The listening page: a prompt, the voice as a row of rounded bars that
   scroll in from the right (older ones fading), the time so far, a hint. */
#define WAVE_Y   250          /* centre line */
#define WAVE_H   92           /* tallest half-bar */
#define BAR_W    5
#define BAR_GAP  3

static void listening(canvas_t *c, const ui_view_t *v)
{
    /* In a conversation the page moves up to make room for the buttons. */
    int ty = v->convo ? 74 : 96, wy = v->convo ? 194 : WAVE_Y, wh = v->convo ? 66 : WAVE_H;
    aafont_draw(c, &aafont_rooms_head, W / 2, ty, v->title && v->title[0] ? v->title : "Go ahead, I'm listening",
                C_TEXT, AAFONT_CENTRE);
    int span = UI_WAVE_BARS * (BAR_W + BAR_GAP) - BAR_GAP;
    int x0 = (W - span) / 2;
    uint16_t hot = RGB(0xFF, 0x6B, 0x5E), cold = RGB(0x3A, 0x22, 0x24);
    for (int i = 0; i < UI_WAVE_BARS; i++) {
        /* Bar i shows level k: the newest at the right, empty ones flat. */
        int k = v->nlevels - UI_WAVE_BARS + i;
        int lv = v->levels && k >= 0 && k < v->nlevels ? v->levels[k] : 0;
        int half = 3 + lv * wh / 255;
        float age = (float)(UI_WAVE_BARS - 1 - i) / (UI_WAVE_BARS - 1);   /* 0 newest .. 1 oldest */
        uint16_t col = mix(hot, cold, age * 0.8f);
        int x = x0 + i * (BAR_W + BAR_GAP);
        /* A pill: the body, then each end a pixel narrower, so it reads round. */
        canvas_fill_rect(c, x, wy - half + 1, BAR_W, 2 * half - 2, col);
        canvas_fill_rect(c, x + 1, wy - half, BAR_W - 2, 1, col);
        canvas_fill_rect(c, x + 1, wy + half - 1, BAR_W - 2, 1, col);
    }
    char t[24];
    int s10 = v->listen_ms / 100;
    snprintf(t, sizeof t, "%d.%d s", s10 / 10, s10 % 10);
    aafont_draw(c, &aafont_inter_sub, W / 2, wy + wh + 30, t, C_DIM, AAFONT_CENTRE | AAFONT_ADVANCE);
    if (!v->convo) {
        aafont_draw(c, &aafont_rooms_small, W / 2, H - 52, "pause when you're done  -  tap to end", C_DIM, AAFONT_CENTRE);
        return;
    }
    aafont_draw(c, &aafont_inter_sub, W / 2, CONVO_Y - 40, "or just keep talking", C_DIM, AAFONT_CENTRE);
    static const char *const word[3] = { "Again", "Slower", "Done" };
    static const char *const what[3] = { "say it again", "repeat slowly", "I'm finished" };
    for (int i = 0; i < 3; i++)
        big_button(c, CONVO_X0 + i * (CONVO_W + 12), CONVO_Y, CONVO_W, CONVO_H, word[i], what[i], C_BTN);
}

/* A question that wants a yes: what Claude means to do, large, and two big
   buttons. The mics stay open, so saying it works as well. */
static void confirm_card(canvas_t *c, const ui_view_t *v)
{
    aafont_draw(c, &aafont_rooms_head, W / 2, 70, "Shall I go ahead?", C_TEXT, AAFONT_CENTRE);
    int pitch;
    const aafont_t *f = answer_face(v, &pitch);
    int max = lines_fit(f, pitch, 120, YN_Y - 8);
    para(c, f, v->confirm, 40, 120, W - 80, pitch, max < 4 ? max : 4, C_TEXT);
    big_button(c, YES_X, YN_Y, YN_W, YN_H, "Yes", NULL, C_GREEN);
    big_button(c, NO_X, YN_Y, YN_W, YN_H, "No", NULL, C_NO);
    aafont_draw(c, &aafont_inter_sub, W / 2, YN_Y + YN_H + 16, "or just say yes or no", C_DIM, AAFONT_CENTRE);
}

/* An answer in the chosen face, from y down to bottom; captions when
   `spoken` >= 0. Returns the y below it. */
static int answer_text(canvas_t *c, const ui_view_t *v, const char *t, int y, int bottom, int spoken)
{
    int pitch;
    const aafont_t *f = answer_face(v, &pitch);
    int max = lines_fit(f, pitch, y, bottom);
    if (!v->textsize && max > 8) max = 8;
    uint16_t dim = contrast ? RGB(0x80, 0x73, 0x00) : C_SAID;
    return para_hl(c, f, t, 24, y, W - 48, pitch, max, C_TEXT, spoken, dim);
}

static void speaking(canvas_t *c, const ui_view_t *v)
{
    int y = para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 2, C_DIM);
    bool has_note = v->note && v->note[0];
    int bottom = v->convo ? (has_note ? STOP_Y - 44 : STOP_Y - 16) : (has_note ? H - 44 : H - 10);
    /* large: easy to read at arm's length */
    answer_text(c, v, v->text, y + 24, bottom, v->mode == UI_SPEAKING ? v->spoken : -1);
    if (!v->convo) {
        note(c, v->note);
        return;
    }
    if (has_note) aafont_draw(c, &aafont_rooms_small, W / 2, STOP_Y - 30, v->note, C_AMBER, AAFONT_CENTRE);
    big_button(c, STOP_X, STOP_Y, STOP_W, STOP_H, "Stop", NULL, C_BTN);
}

/* ---- cards: a message from family, a reminder ---- */

static void envelope(canvas_t *c, int x, int y, uint16_t col, uint16_t bg)
{
    canvas_fill_rect(c, x, y, 34, 24, col);
    for (int i = 0; i < 16; i++) {                       /* the flap: a V from the top corners */
        canvas_fill_rect(c, x + 1 + i, y + 1 + i * 13 / 16, 2, 2, bg);
        canvas_fill_rect(c, x + 31 - i, y + 1 + i * 13 / 16, 2, 2, bg);
    }
}

static void bell(canvas_t *c, int x, int y, uint16_t col)
{
    canvas_disc(c, x + 17, y + 11, 11, col);            /* the dome */
    canvas_fill_rect(c, x + 6, y + 11, 23, 10, col);     /* its sides */
    canvas_fill_rect(c, x + 2, y + 20, 31, 4, col);      /* the lip */
    canvas_disc(c, x + 17, y + 28, 3, col);              /* the clapper */
    canvas_disc(c, x + 17, y, 2, col);                   /* the loop on top */
}

static void card_page(canvas_t *c, const ui_view_t *v)
{
    bool msg = v->mode == UI_MESSAGE;
    const char *title = v->title && v->title[0] ? v->title : msg ? "Message" : "Reminder";
    /* The icon and the title centred together as one. */
    const aafont_t *hf = &aafont_rooms_head;
    char t[64];
    snprintf(t, sizeof t, "%s", title);
    while (t[0] && aafont_width(hf, t) > W - 48 - 48) t[strlen(t) - 1] = 0;
    int w = 34 + 14 + aafont_width(hf, t), x = (W - w) / 2, ty = 70;
    if (msg) envelope(c, x, ty - 1, C_TEXT, C_BG);
    else bell(c, x, ty - 3, C_AMBER);
    aafont_draw(c, hf, x + 48, ty, t, C_TEXT, AAFONT_LEFT);
    canvas_fill_rect(c, 24, ty + hf->cap + 22, W - 48, 1, C_LINE);
    bool has_note = v->note && v->note[0];
    answer_text(c, v, v->text, ty + hf->cap + 46, has_note ? OK_Y - 44 : OK_Y - 16, -1);
    if (has_note) aafont_draw(c, &aafont_rooms_small, W / 2, OK_Y - 30, v->note, C_AMBER, AAFONT_CENTRE);
    big_button(c, OK_X, OK_Y, OK_W, OK_H, "OK", NULL, C_BLUE);
}

/* The resting orb: a core and a glow that fades into the black, drawn as
   rings from the outside in, each a little brighter. */
static void orb(canvas_t *c, int cx, int cy, bool amber)
{
    const int glow = 96, core = 30;
    int R = amber ? 0xF5 : 0x4F, G = amber ? 0xA8 : 0x8C, B = amber ? 0x2E : 0xFF;
    for (int r = glow; r > core; r -= 2) {
        float f = (float)(glow - r) / (glow - core);    /* 0 at the edge, 1 at the core */
        f = f * f * 0.55f;
        canvas_disc(c, cx, cy, r, RGB((int)(R * f), (int)(G * f), (int)(B * f)));
    }
    canvas_disc(c, cx, cy, core, RGB(R, G, B));
    canvas_disc(c, cx - 9, cy - 9, 9, RGB((R + 255) / 2, (G + 255) / 2, (B + 255) / 2));   /* a highlight */
}

void ui_day_words(int wday, int month, int mday, int minute, char *part, size_t np, char *date, size_t nd)
{
    static const char *const days[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                            "August", "September", "October", "November", "December" };
    if (np) part[0] = 0;
    if (nd) date[0] = 0;
    if (month < 1 || month > 12) return;
    int m = ((minute % 1440) + 1440) % 1440;
    const char *when = m >= 5 * 60 && m < 12 * 60 ? "morning" : m >= 12 * 60 && m < 17 * 60 ? "afternoon"
                     : m >= 17 * 60 && m < 21 * 60 ? "evening" : "night";
    if (np) {
        if (wday >= 0 && wday < 7) snprintf(part, np, "%s %s", days[wday], when);
        else snprintf(part, np, "%s", when);
    }
    if (nd) {
        if (mday > 0) snprintf(date, nd, "%s %d", months[month - 1], mday);
        else snprintf(date, nd, "%s", months[month - 1]);
    }
}

static void resting(canvas_t *c, const ui_view_t *v)
{
    canvas_fill_rect(c, 0, 0, W, H, RGB(0, 0, 0));
    orb(c, W / 2, 196, v->amber);
    if (v->now >= 0) {
        char t[24];
        snprintf(t, sizeof t, "%d:%02d", v->now / 60 % 24, v->now % 60);
        aafont_draw(c, &aafont_rooms_head, W / 2, 322, t, C_CLOCK, AAFONT_CENTRE | AAFONT_ADVANCE);
        /* And the day in words, for when the date has slipped away. */
        char part[32], date[32];
        ui_day_words(v->wday, v->month, v->mday, v->now, part, sizeof part, date, sizeof date);
        aafont_draw(c, &aafont_inter_sub, W / 2, 370, part, C_CLOCK, AAFONT_CENTRE);
        aafont_draw(c, &aafont_inter_sub, W / 2, 402, date, C_CLOCK, AAFONT_CENTRE);
    }
}

void ui_draw(canvas_t *c, const ui_view_t *v)
{
    contrast = v->contrast;
    C_BG   = contrast ? RGB(0, 0, 0) : RGB(0x0E, 0x10, 0x14);
    C_TEXT = contrast ? RGB(0xFF, 0xE6, 0x00) : RGB(0xF2, 0xF4, 0xF7);
    C_DIM  = contrast ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x8A, 0x92, 0x9E);
    if (v->mode == UI_RESTING) {
        resting(c, v);
        if (v->mic_open) banner(c);
        return;
    }
    canvas_fill_rect(c, 0, 0, W, H, C_BG);
    header(c, v);
    const aafont_t *sub = &aafont_inter_sub;
    switch (v->mode) {
    case UI_HOME:
        talk_button(c, C_BLUE, "Talk", "tap, or press BOOT");
        note(c, v->note);
        gear(c, GEAR_X, GEAR_Y);
        break;
    case UI_SETTINGS:
        settings(c, v);
        break;
    case UI_VOICES:
        voices_page(c, v);
        break;
    case UI_EASY:
        easy(c, v);
        break;
    case UI_LISTENING:
        if (v->confirm && v->confirm[0]) confirm_card(c, v);
        else listening(c, v);
        break;
    case UI_THINKING:
        para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 3, C_DIM);
        aafont_draw(c, &aafont_rooms_head, W / 2, H / 2, "Thinking...", C_TEXT, AAFONT_CENTRE);
        note(c, v->note);
        break;
    case UI_SPEAKING:
    case UI_ANSWER:
        speaking(c, v);
        break;
    case UI_MESSAGE:
    case UI_REMINDER:
        card_page(c, v);
        break;
    case UI_ERROR:
        aafont_draw(c, &aafont_rooms_head, W / 2, 150, v->title ? v->title : "Something went wrong", C_RED, AAFONT_CENTRE);
        para(c, sub, v->text, 24, 210, W - 48, 32, 6, C_TEXT);
        break;
    case UI_AGENT_TEXT: {
        int y = 70;
        if (v->title && v->title[0]) y = para(c, &aafont_rooms_head, v->title, 24, 70, W - 48, 38, 2, C_TEXT) + 10;
        para(c, sub, v->text, 24, y, W - 48, 32, 11, C_TEXT);
        break;
    }
    case UI_RESTING:                       /* drawn above */
        break;
    case UI_AGENT_IMAGE:
        if (v->image && v->img_w > 0 && v->img_h > 0 && v->img_w <= W && v->img_h <= H)
            canvas_blit(c, v->image, v->img_w, v->img_h, (W - v->img_w) / 2, (H - v->img_h) / 2);
        break;
    }
    if (v->mic_open) banner(c);         /* last, over everything: the microphones are open */
}
