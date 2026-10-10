#include "ui.h"

#include <math.h>

#include <stdio.h>
#include <string.h>

#include "aafont.h"

extern const aafont_t aafont_rooms_head;    /* Inter SemiBold 30 */
extern const aafont_t aafont_rooms_title;   /* Inter SemiBold 17 */
extern const aafont_t aafont_rooms_small;   /* Inter Medium 15 */

#define W 480
#define H 480
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BG     RGB(0x0E, 0x10, 0x14)
#define C_TEXT   RGB(0xF2, 0xF4, 0xF7)
#define C_DIM    RGB(0x8A, 0x92, 0x9E)
#define C_AMBER  RGB(0xF5, 0xB0, 0x41)
#define C_BLUE   RGB(0x3B, 0x6F, 0xD0)
#define C_RED    RGB(0xD9, 0x3A, 0x3A)
#define C_BANNER RGB(0xE0, 0x1E, 0x1E)

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
   Returns the y below the last line. */
static int para(canvas_t *c, const aafont_t *f, const char *s, int x, int y, int maxw, int pitch, int max, uint16_t col)
{
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
        aafont_draw(c, f, x, y, out, col, AAFONT_LEFT);
        y += pitch;
        lines++;
    }
    return y;
}

static void header(canvas_t *c, const ui_view_t *v)
{
    aafont_draw(c, &aafont_rooms_small, 16, 18, "stan-claw", C_DIM, AAFONT_LEFT);
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

/* A rounded button with centred text; `on` lights it. */
static void button(canvas_t *c, int x, int y, int w, int h, const char *t, bool on)
{
    uint16_t bg = on ? C_BLUE : C_BTN;
    canvas_fill_rect(c, x + 3, y, w - 6, h, bg);
    canvas_fill_rect(c, x, y + 3, w, h - 6, bg);
    canvas_disc(c, x + 3, y + 3, 3, bg);
    canvas_disc(c, x + w - 4, y + 3, 3, bg);
    canvas_disc(c, x + 3, y + h - 4, 3, bg);
    canvas_disc(c, x + w - 4, y + h - 4, 3, bg);
    aafont_draw(c, &aafont_rooms_title, x + w / 2, y + (h - aafont_rooms_title.cap) / 2, t, on ? C_TEXT : C_DIM, AAFONT_CENTRE);
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
#define SPEED_Y       292
#define MODEL_Y       382
#define SEG_H         44

static void label(canvas_t *c, int y, const char *t)
{
    aafont_draw(c, &aafont_rooms_small, 40, y, t, C_DIM, AAFONT_LEFT);
}

static void settings(canvas_t *c, const ui_view_t *v)
{
    topbar(c, "Settings");
    /* Voice: name, a hint, a chevron to the list. */
    label(c, ROW_VOICE_Y + 16, "VOICE");
    aafont_draw(c, &aafont_inter_sub, 40, ROW_VOICE_Y + 40, v->voice_name && v->voice_name[0] ? v->voice_name : "not chosen",
                C_TEXT, AAFONT_LEFT);
    for (int i = 0; i < 8; i++) {
        canvas_fill_rect(c, 432 + i, ROW_VOICE_Y + 44 - 8 + i, 2, 2, C_DIM);
        canvas_fill_rect(c, 432 + i, ROW_VOICE_Y + 44 + 8 - i, 2, 2, C_DIM);
    }
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
    if (v->mode != UI_SETTINGS && v->mode != UI_VOICES) return UI_HIT_NONE;
    if (y < TOP_H && x < 160) return UI_HIT_BACK;
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
    aafont_draw(c, &aafont_rooms_head, W / 2, 96, "Go ahead, I'm listening", C_TEXT, AAFONT_CENTRE);
    int span = UI_WAVE_BARS * (BAR_W + BAR_GAP) - BAR_GAP;
    int x0 = (W - span) / 2;
    uint16_t hot = RGB(0xFF, 0x6B, 0x5E), cold = RGB(0x3A, 0x22, 0x24);
    for (int i = 0; i < UI_WAVE_BARS; i++) {
        /* Bar i shows level k: the newest at the right, empty ones flat. */
        int k = v->nlevels - UI_WAVE_BARS + i;
        int lv = v->levels && k >= 0 && k < v->nlevels ? v->levels[k] : 0;
        int half = 3 + lv * WAVE_H / 255;
        float age = (float)(UI_WAVE_BARS - 1 - i) / (UI_WAVE_BARS - 1);   /* 0 newest .. 1 oldest */
        uint16_t col = mix(hot, cold, age * 0.8f);
        int x = x0 + i * (BAR_W + BAR_GAP);
        /* A pill: the body, then each end a pixel narrower, so it reads round. */
        canvas_fill_rect(c, x, WAVE_Y - half + 1, BAR_W, 2 * half - 2, col);
        canvas_fill_rect(c, x + 1, WAVE_Y - half, BAR_W - 2, 1, col);
        canvas_fill_rect(c, x + 1, WAVE_Y + half - 1, BAR_W - 2, 1, col);
    }
    char t[24];
    int s10 = v->listen_ms / 100;
    snprintf(t, sizeof t, "%d.%d s", s10 / 10, s10 % 10);
    aafont_draw(c, &aafont_inter_sub, W / 2, WAVE_Y + WAVE_H + 34, t, C_DIM, AAFONT_CENTRE | AAFONT_ADVANCE);
    aafont_draw(c, &aafont_rooms_small, W / 2, H - 52, "pause when you're done", C_DIM, AAFONT_CENTRE);
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

static void resting(canvas_t *c, const ui_view_t *v)
{
    canvas_fill_rect(c, 0, 0, W, H, RGB(0, 0, 0));
    orb(c, W / 2, 210, v->amber);
    if (v->now >= 0) {
        char t[24];
        snprintf(t, sizeof t, "%d:%02d", v->now / 60 % 24, v->now % 60);
        aafont_draw(c, &aafont_rooms_head, W / 2, 340, t, RGB(0x5A, 0x60, 0x6A), AAFONT_CENTRE | AAFONT_ADVANCE);
    }
}

void ui_draw(canvas_t *c, const ui_view_t *v)
{
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
    case UI_LISTENING:
        listening(c, v);
        break;
    case UI_THINKING:
        para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 3, C_DIM);
        aafont_draw(c, &aafont_rooms_head, W / 2, H / 2, "Thinking...", C_TEXT, AAFONT_CENTRE);
        note(c, v->note);
        break;
    case UI_SPEAKING:
    case UI_ANSWER: {
        int y = para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 2, C_DIM);
        para(c, sub, v->text, 24, y + 22, W - 48, 32, 10, C_TEXT);
        note(c, v->note);
        break;
    }
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
