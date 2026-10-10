#include "ui.h"

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
