#include "noiseui.h"

#include "aafont.h"
#include "ring.h"
#include "vector.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Every y below is the top of a line's capitals, as in envui, so two pieces
 * of text said to line up do whatever their sizes; where two sizes share a
 * line, the line is a baseline -- the top plus the face's cap.
 */

/* ---- palette ---------------------------------------------------------------
 * The watch face's (face.c), so the page reads as the same watch: cream for
 * what is printed, a softer cream for units, gold for labels as the moon page
 * sets NEXT FULL, and its gilt rule; a blue-grey rail for the strip's floor.
 * The level's colour is the only other colour on the page, and it only ever
 * means the level. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define CREAM       RGB(236, 228, 206)
#define CREAM_SOFT  RGB(196, 190, 172)
#define CREAM_DIM   RGB(140, 136, 122)
#define GOLD        RGB(224, 186, 118)
#define GOLD_LO     RGB(156, 116, 60)
#define RAIL        RGB(84, 90, 108)
#define HATCH       RGB(52, 58, 74)
#define HALO        RGB(4, 6, 14)

/* The ground: the moon page's night sky without the moon -- a deep navy at
   the top settling to near black at the foot -- ordered-dithered, since a
   navy this dark has four or five levels of blue in 565 and undithered it
   comes out as bands. Channels 0..255, at the top and at the foot. */
static const float s_sky_top[3] = { 9.0f, 14.0f, 34.0f };
static const float s_sky_foot[3] = { 3.0f, 5.0f, 13.0f };

/* ---- layout ---------------------------------------------------------------
 * The panel's corners are rounded, R5 mm on the glass -- 43 px at its
 * 0.1166 mm pitch -- so nothing is drawn within 12 px of the left or right
 * edge, and nothing within 8 px of a corner's arc: the strip and the label
 * under it stop 25 px above the foot, where the label's first letter clears
 * the bottom-left arc by that much. There is no title: a large figure with
 * dBA beside it says what the page is, and the room a title would take is
 * the strip's. */
#define XR          228            /* one past the last column text may use */
#define MID         120

#define NUM_BASE    84             /* the number's capitals 27..83 */
#define UNIT_GAP    8              /* the number's box to the unit's ink */
#define NO_SIG_TOP  54             /* NO SIGNAL, in the number's place: 54..78 */
#define WORD_TOP    100            /* QUIET .. VERY LOUD, capitals 100..116 */
#define RULE_Y      129.5f         /* the gilt rule, on row 129 */
#define TODAY_BASE  158            /* TODAY and its figure stand on this */

/* The strip: 180 columns, two of the history's 10 s points each, over 60
   rows, one per dB from 30 to 90; the quarter-hour ticks under its floor,
   and LAST HOUR under those. */
#define S_X0        16
#define S_COLS      180
#define S_FLOOR     234            /* the floor row; the plot is rows 174..233 */
#define S_ROWS      60
#define S_LO        30.0f
#define S_HI        90.0f
#define S_LABEL_TOP 242            /* LAST HOUR, capitals 242..254 */

#define F_LABEL     (&aafont_inter_label)     /* capitals 13 */
#define F_SUB       (&aafont_inter_sub)       /* 17 */
#define F_WORD      (&aafont_inter_word)      /* 25 */
#define F_READING   (&aafont_inter_reading)   /* 57 */

/* ---- the level --------------------------------------------------------------- */

noiseui_state_t noiseui_state_for(float dba)
{
    if (!(dba >= 45.0f)) return NOISEUI_QUIET;     /* NaN lands here too */
    if (dba < 60.0f) return NOISEUI_MODERATE;
    if (dba < 75.0f) return NOISEUI_LOUD;
    return NOISEUI_VERY_LOUD;
}

const char *noiseui_state_name(noiseui_state_t st)
{
    switch (st) {
    case NOISEUI_MODERATE:  return "MODERATE";
    case NOISEUI_LOUD:      return "LOUD";
    case NOISEUI_VERY_LOUD: return "VERY LOUD";
    default:                return "QUIET";
    }
}

/*
 * ring_colour_for gives light, 0..1 a channel, because a WS2812 turns bytes
 * straight into light. A panel does not: its codes are gamma-encoded, so the
 * light goes back through the 2.2 power the ring took it through, to the
 * sRGB byte the spec gave the stop, and from there to 565 as every colour in
 * the codebase goes (RGB above). At the stops that is exact -- 45 dBA is
 * (0, 200, 60) -- and between them it is the ring's linear-light blend, which
 * does not sag in the middle as mixing the codes would.
 */
uint16_t noiseui_colour(float dba)
{
    float lin[3];
    ring_colour_for(dba, lin);
    int b[3];
    for (int k = 0; k < 3; k++) {
        float v = lin[k] > 0.0f ? powf(lin[k], 1.0f / 2.2f) * 255.0f : 0.0f;
        b[k] = v >= 255.0f ? 255 : (int)(v + 0.5f);
    }
    return RGB(b[0], b[1], b[2]);
}

/* The same with its light scaled by `k`: the stops' faint lines. */
static uint16_t colour_dim(float dba, float k)
{
    float lin[3];
    ring_colour_for(dba, lin);
    int b[3];
    for (int m = 0; m < 3; m++) {
        float v = lin[m] * k;
        v = v > 0.0f ? powf(v, 1.0f / 2.2f) * 255.0f : 0.0f;
        b[m] = v >= 255.0f ? 255 : (int)(v + 0.5f);
    }
    return RGB(b[0], b[1], b[2]);
}

/* A level worth drawing: a finite number of dBA. Anything else is not a
   reading and draws no number, no word and no colour. */
static bool level_ok(float v) { return isfinite(v); }

/*
 * A level as the page prints it: whole decibels, rounded, held to 0..199 so
 * the figure is never more than three digits whatever arrives. Rounded, not
 * truncated as envui's readings are: envui truncates so its number never
 * shows a limit its word has not reached, and here the word is not read off
 * this figure at all but off LAeq,3s.
 */
static int whole_db(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 199.0f) return 199;
    return (int)floorf(v + 0.5f);
}

/* ---- drawing helpers --------------------------------------------------------- */

static void pixel(canvas_t *c, int x, int y, uint16_t col)
{
    if (x >= 0 && y >= 0 && x < c->w && y < c->h) c->fb[y * c->w + x] = col;
}

/* `col` laid over what is there at `a` of 1, in linear light, as the type's
   edges are laid. */
static void tint(canvas_t *c, int x, int y, uint16_t col, float a)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h || !(a > 0.0f)) return;
    uint16_t *p = &c->fb[y * c->w + x];
    *p = vec_blend_linear(*p, col, (uint8_t)(a >= 1.0f ? 255 : (int)(a * 255.0f + 0.5f)));
}

static const uint8_t s_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };

static void ground(canvas_t *c)
{
    for (int j = 0; j < c->h; j++) {
        float t = c->h > 1 ? (float)j / (float)(c->h - 1) : 0.0f;
        /* Eased, so the navy holds a while behind the number before it
           settles, rather than fading at one rate all the way down. */
        t = t * t * (3.0f - 2.0f * t);
        float ch[3];
        for (int k = 0; k < 3; k++) ch[k] = s_sky_top[k] + (s_sky_foot[k] - s_sky_top[k]) * t;
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        for (int i = 0; i < c->w; i++) {
            float d = ((float)s_bayer[((j & 3) << 2) | (i & 3)] + 0.5f) / 16.0f;
            int r5 = (int)(ch[0] / 255.0f * 31.0f + d);
            int g6 = (int)(ch[1] / 255.0f * 63.0f + d);
            int b5 = (int)(ch[2] / 255.0f * 31.0f + d);
            row[i] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        }
    }
}

/*
 * Capitals set with extra space between them, as the watch sets its small
 * labels (the moon page's NEXT FULL): a label spaced out reads as a label,
 * not as a word in a sentence. `track` px between letters, over the face's
 * own spacing. Returns the ink's width; draws only when `c` is not NULL.
 * Placed by the ink, like aafont: `x` is its left edge, centre or right edge
 * by `align`.
 */
static int tracked(canvas_t *c, const aafont_t *f, int x, int y, const char *s,
                   uint16_t col, int align, int track)
{
    /* The pen before each letter, and the ink's extent from the first pen. */
    int pen = 0, left = 0, right = 0;
    bool any = false;
    for (const char *p = s; *p != '\0'; p++) {
        char one[2] = { *p, '\0' };
        int w = aafont_width(f, one);
        if (w > 0) {
            int l = pen + aafont_bearing(f, one);
            if (!any || l < left) left = l;
            if (!any || l + w > right) right = l + w;
            any = true;
        }
        pen += aafont_advance(f, one) + track;
    }
    if (!any) return 0;
    int width = right - left;
    if (c == NULL) return width;
    int x0 = align == AAFONT_CENTRE ? x - width / 2 : align == AAFONT_RIGHT ? x - width : x;
    pen = x0 - left;
    for (const char *p = s; *p != '\0'; p++) {
        char one[2] = { *p, '\0' };
        aafont_draw(c, f, pen, y, one, col, AAFONT_LEFT | AAFONT_ADVANCE);
        pen += aafont_advance(f, one) + track;
    }
    return width;
}

/* ---- the number, the word, today ----------------------------------------------- */

/*
 * The level and its unit, centred as one group: the figure, then a column
 * holding EST over dBA -- dBA standing on the figure's baseline, EST (when
 * the level is an estimate) level with the figure's top, so the mark sits
 * where a footnote would and changes nothing else when it comes and goes.
 * The figure is placed by its advance box, not its ink: the digits are
 * tabular, so 58 turning to 61 changes the digits and nothing moves.
 */
static void number(canvas_t *c, const noiseui_t *s)
{
    char b[8];
    bool ok = level_ok(s->laf);
    if (ok) snprintf(b, sizeof b, "%d", whole_db(s->laf));
    else snprintf(b, sizeof b, "--");

    const aafont_t *f = F_READING;
    int adv = aafont_advance(f, b), unit = aafont_width(F_SUB, "dBA");
    int x = MID - (adv + UNIT_GAP + unit) / 2;
    uint16_t col = !ok ? CREAM_DIM : level_ok(s->laeq3) ? noiseui_colour(s->laeq3) : CREAM;
    aafont_draw(c, f, x, NUM_BASE - f->cap, b, col, AAFONT_LEFT | AAFONT_ADVANCE);

    int ux = x + adv + UNIT_GAP;
    aafont_draw(c, F_SUB, ux, NUM_BASE - F_SUB->cap, "dBA", CREAM_SOFT, AAFONT_LEFT);
    if (!s->calibrated)
        tracked(c, F_LABEL, ux, NUM_BASE - f->cap, "EST", GOLD, AAFONT_LEFT, 1);
}

/* The word for LAeq,3s, spaced like the labels but a size up and in the
   level's colour -- colour never alone, the word always with it. */
static void word(canvas_t *c, const noiseui_t *s)
{
    if (!level_ok(s->laeq3)) return;
    tracked(c, F_SUB, MID, WORD_TOP, noiseui_state_name(noiseui_state_for(s->laeq3)),
            noiseui_colour(s->laeq3), AAFONT_CENTRE, 3);
}

/* The moon page's gilt rule, a short line either side of a lozenge: it
   closes the reading above it off from the day and the hour below. */
static void rule(canvas_t *c)
{
    vec_line(c, MID - 30.0f, RULE_Y, MID - 5.0f, RULE_Y, 0.6f, GOLD_LO);
    vec_line(c, MID + 5.0f, RULE_Y, MID + 30.0f, RULE_Y, 0.6f, GOLD_LO);
    float dia[8] = { MID, RULE_Y - 2.2f, MID + 2.2f, RULE_Y, MID, RULE_Y + 2.2f, MID - 2.2f, RULE_Y };
    vec_polygon(c, dia, 4, GOLD);
}

/*
 * Today's average: TODAY in gold, then speaker's LAeq since midnight -- the
 * figure in cream a size up, its unit beside it -- on one baseline and
 * centred as a group. "--" when speaker does not know it yet (a board just
 * started, a card that is not there) or when there is no signal: an average
 * from a line that has stopped coming is a number that has stopped being
 * true, like any other.
 */
static void today(canvas_t *c, const noiseui_t *s)
{
    char b[8];
    bool ok = s->have_signal && level_ok(s->today);
    if (ok) snprintf(b, sizeof b, "%d", whole_db(s->today));
    else snprintf(b, sizeof b, "--");
    int lw = tracked(NULL, F_LABEL, 0, 0, "TODAY", GOLD, AAFONT_LEFT, 2);
    int vw = aafont_width(F_SUB, b), uw = ok ? aafont_width(F_LABEL, "dBA") : 0;
    int gap = 10, ugap = ok ? 7 : 0;
    int x = MID - (lw + gap + vw + ugap + uw) / 2;
    tracked(c, F_LABEL, x, TODAY_BASE - F_LABEL->cap, "TODAY", GOLD, AAFONT_LEFT, 2);
    x += lw + gap;
    aafont_draw(c, F_SUB, x, TODAY_BASE - F_SUB->cap, b, ok ? CREAM : CREAM_DIM, AAFONT_LEFT);
    if (ok) aafont_draw(c, F_LABEL, x + vw + ugap, TODAY_BASE - F_LABEL->cap, "dBA", CREAM_SOFT, AAFONT_LEFT);
}

/* ---- the strip ----------------------------------------------------------------- */

/* Height above the floor, in px, for a level on the fixed 30..90 scale.
   Below 30 is the floor itself; above 90 is the top. */
static float strip_h(float dba)
{
    float f = (dba - S_LO) / (S_HI - S_LO);
    if (!(f > 0.0f)) f = 0.0f;
    if (f > 1.0f) f = 1.0f;
    return f * (float)S_ROWS;
}

static bool point_ok(const noiseui_t *s, int i)
{
    return s->hist_valid[i] && level_ok(s->hist[i]);
}

/*
 * Each column's level: the louder of its two points, so a loud 10 s is drawn
 * at its own height rather than averaged down with the quiet one beside it.
 * A column with neither point is a gap.
 */
static bool column(const noiseui_t *s, int col, float *out)
{
    bool any = false;
    float best = 0.0f;
    for (int i = col * 2; i < col * 2 + 2 && i < NOISEUI_POINTS; i++) {
        if (!point_ok(s, i)) continue;
        if (!any || s->hist[i] > best) best = s->hist[i];
        any = true;
    }
    *out = best;
    return any;
}

/*
 * The level round a column, for the wash's colour: the median of the valid
 * columns among the five centred on it. A median, not a mean, so one
 * slammed door -- a single column 16 dB over its neighbours -- does not tint
 * the wash for two columns either side of it; a stretch that is loud for
 * longer than that does.
 */
static float around(const float *lv, const bool *have, int k)
{
    float v[5];
    int n = 0;
    for (int j = k - 2; j <= k + 2; j++)
        if (j >= 0 && j < S_COLS && have[j]) {
            int i = n++;
            while (i > 0 && v[i - 1] > lv[j]) { v[i] = v[i - 1]; i--; }
            v[i] = lv[j];
        }
    return n > 0 ? (n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2])) : lv[k];
}

/*
 * The last hour: a line along the top, each column in the colour of its own
 * level, as the number is, over a wash in the colour of the level round it
 * that fades towards the floor -- so a loud stretch is red from its top to
 * the floor rather than red at the top of a column green underneath, the
 * line is what the eye follows, and the wash only says how high it stood. The
 * ring's four stops are drawn across it as faint dotted lines in their own
 * colours, 45 and 75 with their figures beside them: how loud it got is read
 * against the ring's own scale. A gap, from the first point on, is hatched:
 * no line from speaker is not a quiet room. Before the first point there is
 * nothing, not an hour of apparent failure.
 */
/* Each column's level and whether it has one, worked out once a draw.
   Static rather than on the stack, as envui's are: drawing allocates
   nothing, and the page is drawn from one task (vector.c's polygon rows
   make it single-task anyway). */
static float s_lv[S_COLS];
static bool s_have[S_COLS];

static void strip(canvas_t *c, const noiseui_t *s)
{
    int top = S_FLOOR - S_ROWS;
    int first = -1;
    float *lv = s_lv;
    bool *have = s_have;
    for (int k = 0; k < S_COLS; k++) {
        have[k] = column(s, k, &lv[k]);
        if (have[k] && first < 0) first = k;
    }

    static const float stop[4] = { 45.0f, 55.0f, 65.0f, 75.0f };
    for (int k = 0; k < 4; k++) {
        int y = S_FLOOR - (int)lroundf(strip_h(stop[k]));
        uint16_t col = colour_dim(stop[k], 0.30f);
        for (int x = S_X0; x < S_X0 + S_COLS; x += 3) pixel(c, x, y, col);
    }

    for (int k = 0; k < S_COLS; k++) {
        int x = S_X0 + k;
        if (!have[k]) {
            if (first >= 0 && k > first)
                for (int y = top; y < S_FLOOR; y++)
                    if ((x + y) % 5 == 0) pixel(c, x, y, HATCH);
            continue;
        }
        /*
         * The wash: the colour laid under the line at a strength set by the
         * row's height on the scale -- a twentieth of its light at the
         * floor, a third at the top -- so it is one fixed gradient that the
         * line uncovers, and a loud stretch washes brighter than a quiet one
         * as well as higher. Faded by each column's own height instead, a
         * slammed door's column was dimmer than its neighbours at every row
         * they shared, a dark streak from its peak to the floor. Its colour
         * is the level round it (around, above), not its own, which streaked
         * it wherever the level hovered at a colour's edge. The line keeps
         * each column's own.
         */
        float h = strip_h(lv[k]);
        uint16_t col = noiseui_colour(around(lv, have, k));
        for (int y = S_FLOOR - 1; y >= top; y--) {
            float above = (float)(S_FLOOR - y);           /* the row's top edge, from the floor */
            float cover = h - (above - 1.0f);             /* how much of the row the column fills */
            if (cover <= 0.0f) break;
            if (cover > 1.0f) cover = 1.0f;
            tint(c, x, y, col, cover * (0.05f + 0.28f * (above - 0.5f) / (float)S_ROWS));
        }
    }

    /* The line along the top, joined column to column; a column with gaps
       both sides is a dot. Each joint in the colour of the louder end, so a
       loud 10 s shows in its own colour on both sides of its peak. */
    for (int k = 0; k < S_COLS; k++) {
        if (!have[k]) continue;
        float x = (float)(S_X0 + k) + 0.5f, y = (float)S_FLOOR - strip_h(lv[k]);
        bool prev = k > 0 && have[k - 1], next = k + 1 < S_COLS && have[k + 1];
        if (next) {
            float y1 = (float)S_FLOOR - strip_h(lv[k + 1]);
            vec_line(c, x, y, x + 1.0f, y1, 1.4f, noiseui_colour(fmaxf(lv[k], lv[k + 1])));
        } else if (!prev) {
            vec_disc(c, x, y, 0.9f, noiseui_colour(lv[k]));
        }
    }

    /* The floor, and a tick under it every quarter hour. */
    canvas_fill_rect(c, S_X0, S_FLOOR, S_COLS, 1, RAIL);
    for (int q = 0; q <= 4; q++) {
        int x = S_X0 + q * S_COLS / 4 - (q == 4 ? 1 : 0);
        canvas_fill_rect(c, x, S_FLOOR + 1, 1, 3, RAIL);
    }
    tracked(c, F_LABEL, S_X0, S_LABEL_TOP, "LAST HOUR", GOLD, AAFONT_LEFT, 2);

    /* 45 and 75, right of the plot, centred on their lines. */
    for (int k = 0; k < 4; k += 3) {
        int y = S_FLOOR - (int)lroundf(strip_h(stop[k]));
        aafont_draw(c, F_LABEL, XR, y - F_LABEL->cap / 2, k == 0 ? "45" : "75", CREAM_DIM, AAFONT_RIGHT);
    }

    /* Now: the live LAeq,3s as a dot just past the strip's end, in its own
       colour, ringed in the ground's dark so it stands off the line it
       ends. Only with a signal: a dot is a claim about now. */
    if (s->have_signal && level_ok(s->laeq3)) {
        float nx = (float)(S_X0 + S_COLS) + 1.5f;
        float ny = (float)S_FLOOR - strip_h(s->laeq3);
        if (ny < (float)top + 3.0f) ny = (float)top + 3.0f;
        if (ny > (float)S_FLOOR - 3.0f) ny = (float)S_FLOOR - 3.0f;
        vec_disc(c, nx, ny, 4.3f, HALO);
        vec_disc(c, nx, ny, 3.0f, noiseui_colour(s->laeq3));
    }
}

/* ---- the page ------------------------------------------------------------------ */

static void page(canvas_t *c, const noiseui_t *s)
{
    ground(c);
    if (s == NULL) return;

    if (s->have_signal) {
        number(c, s);
        word(c, s);
    } else {
        /* In the number's place, a size down so it fits, in the dim cream
           that says "not a reading", in the lower half of the number's band
           so it stays with the line under it -- where the word was, whose
           signal it is -- rather than floating over it. */
        aafont_draw(c, F_WORD, MID, NO_SIG_TOP, "NO SIGNAL", CREAM_DIM, AAFONT_CENTRE);
        tracked(c, F_LABEL, MID, WORD_TOP + 2, "FROM SPEAKER", CREAM_DIM, AAFONT_CENTRE, 2);
    }
    rule(c);
    today(c, s);
    strip(c, s);
}

/*
 * The marks -- the rule, the line, the now-dot -- blend their edges in linear
 * light as the type does (vector.c's default is to mix the codes, which the
 * watch face was judged with and keeps); on for the page, and put back as it
 * was found.
 */
void noiseui_draw(canvas_t *c, const noiseui_t *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    bool was = vec_linear_light(true);
    page(c, s);
    vec_linear_light(was);
}
