#include "face.h"

#include "moonphase.h"
#include "vector.h"
#include "vfont.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * How the face is built.
 *
 * Everything is laid out in design units on a 240x280 portrait panel, the
 * dial's centre at (0, 0), and turned into panel pixels by one scale and
 * offset, so a panel of any other shape gets the same face, smaller.
 *
 * The dial itself -- the sunburst, the three snailed sub-dials, the moon
 * disc seen through its aperture -- is shaded pixel by pixel, in floating
 * point per channel, and then ordered-dithered down to RGB565. The dither
 * matters: a deep navy has only four or five levels of blue in 565's five
 * bits, and an undithered sunburst comes out as a stack of flat rings.
 *
 * With a photo (face_set_photo) the photo takes the sunburst's place at
 * the bottom, set back (see photo_ground), and the sub-dials go over it as
 * smoked discs rather than opaque ones; the moon aperture is the same
 * either way.
 *
 * On top of that go the printed and applied parts, all as anti-aliased
 * vectors: the minute track, the faceted hour batons, each sub-dial's scale
 * and lettering. That is the background, and it depends on nothing but the
 * moon disc's step and the photo. Over a photo the batons, the sub-dials'
 * rings, the small hands and the charging bolt get a fine dark outline;
 * over the sunburst they need none. The hour and minute hands and the cap
 * get it on either dial, since on the sunburst they still cross the gold
 * moons and the reserve's champagne arch.
 *
 * Everything else is drawn over a copy of it every second: the power
 * reserve (track and level, so an unknown reading can show a different
 * track), the day and date hands, today's day set again in gold and any
 * date numeral the date hand lies across, the small seconds, the hour and
 * minute hands and the centre cap. All of that together is a few thousand
 * pixels.
 */

#define PI      3.14159265358979f
#define TWO_PI  6.28318530717959f
#define DEG     (PI / 180.0f)

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) \
                                  | ((b) >> 3)))

/* ---- palette ------------------------------------------------------------
 * Restrained: navy, a warm cream for everything printed or applied, and one
 * gold for everything that moves -- the hands, the moons, and today's day,
 * which is what the day hand is saying. Static metal (the sub-dial rings,
 * the aperture's edge) is a muted bronze, so it frames without competing.
 * The only other colour is the power reserve's red, for the last sixth of
 * the battery, as a gauge would have.
 *
 * The minute track's rails are a blue-grey that sits back into the navy and
 * its ticks a warm grey, so the track reads as a scale of ticks between two
 * faint lines rather than as a ladder of equal bars. */
#define CREAM       RGB(236, 228, 206)
#define CREAM_SOFT  RGB(196, 190, 172)
#define CREAM_DIM   RGB(140, 136, 122)
#define RAIL        RGB(84, 90, 108)
#define TICK        RGB(150, 146, 132)
#define BRONZE      RGB(108, 86, 61)
static const float s_bronze[3] = { 108.0f, 86.0f, 61.0f };  /* for shaders */
#define GOLD_HI     RGB(252, 230, 172)
#define GOLD        RGB(224, 186, 118)
#define GOLD_LO     RGB(156, 116, 60)
#define GOLD_DEEP   RGB(92, 64, 28)
#define SHADOW      RGB(2, 4, 12)
#define RESERVE_BG  RGB(46, 58, 88)
#define RESERVE_RED RGB(168, 52, 44)

/* The light falls from the upper left, for the sunburst's sheen, the facets
   of the hands and batons, and the shadows they cast. */
#define LIGHT_ANGLE (-0.75f * PI)        /* atan2 angle, pointing up-left */
static const float s_light_x = -0.6f, s_light_y = -0.8f;

/* ---- layout, in design units about the dial's centre -------------------- */
#define TRACK_OUT   115.0f      /* the minute track's two rails */
#define TRACK_IN    110.0f
#define BATON_OUT   106.0f      /* the hour batons */
#define BATON_IN     93.0f

/* The sub-dials: as large as they go while keeping 9.5 units clear of the
   batons at 3, 6 and 9 and over 16 clear of one another, since their
   lettering, five units tall, is the smallest thing on the face. */
#define SUB_D        54.0f      /* sub-dial centres from the dial's centre */
#define SUB_R        29.5f      /* sub-dial radius */

/* The moon aperture: a half-disc window of radius MOON_RA whose flat base
   is at MOON_Y, with the disc's pivot at the middle of that base. The two
   moons ride at MOON_RP from the pivot, MOON_RM in radius, and two humps of
   radius MOON_RH, centred on the base at +-MOON_RP, cover each moon at new.
   At full the moon sits at the top with MOON_RA - MOON_RP - MOON_RM of
   blue above it and |(RP, RP)| - RM - RH clear of either hump.

   The humps are only a little larger than the moons, as on a real
   moon-phase disc: that hides a new moon entirely, shows a young crescent
   leaving the hump within a couple of days, and leaves the window a
   half-disc with two bites out of its base. RP + RH stays short of RA, so
   the window keeps a clean foot at either corner rather than tapering to
   a one-pixel horn where a hump meets the arch. The base sits 35 units up,
   so the window is balanced between the 12 and the small seconds. */
#define MOON_Y      (-35.0f)
#define MOON_RA      32.0f
#define MOON_RP      19.0f
#define MOON_RM      10.0f
#define MOON_RH      10.9f

/* The power reserve arches over the aperture, concentric with it and
   close enough to read as its frame: four quarters of a track, reaching
   down round the window's shoulders. */
#define RES_R        36.8f
#define RES_HALF    (66.0f * DEG)
#define RES_W         2.2f
#define RES_LOW       0.16f     /* the red zone, as a fraction */
#define RES_GAP      (1.1f * DEG)   /* half the gap between quarters */

/* The hands. The minute hand's tip reaches into the minute track, between
   its rails, as a minute hand should. */
#define HOUR_LEN     64.0f
#define HOUR_W        4.2f
#define MIN_LEN     112.0f
#define MIN_W         3.3f

typedef struct {
    float cx, cy, s;    /* the dial centre in pixels, and pixels per unit */
} geo_t;

static geo_t geo_for(int w, int h)
{
    geo_t g;
    float sx = (float)w / 240.0f, sy = (float)h / 280.0f;
    g.s = sx < sy ? sx : sy;
    g.cx = 0.5f * (float)w;
    g.cy = 0.5f * (float)h;
    return g;
}

/* A point at radius r and watch angle a (0 up, clockwise) about (ox, oy),
   all in design units, as panel pixels. */
static void polar(const geo_t *g, float ox, float oy, float r, float a,
                  float *x, float *y)
{
    *x = g->cx + (ox + r * sinf(a)) * g->s;
    *y = g->cy + (oy - r * cosf(a)) * g->s;
}

static float px(const geo_t *g, float u) { return g->cx + u * g->s; }
static float py(const geo_t *g, float v) { return g->cy + v * g->s; }

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

static inline float smooth(float e0, float e1, float x)
{
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

static uint16_t mix565(uint16_t a, uint16_t b, float t)
{
    return vec_blend(a, b, (uint8_t)(clamp01(t) * 255.0f + 0.5f));
}

static int wrapi(int v, int n)
{
    v %= n;
    return v < 0 ? v + n : v;
}

/* ---- noise, for the brushing ------------------------------------------- */

static float hash1(uint32_t n)
{
    n = (n << 13) ^ n;
    n = n * (n * n * 15731u + 789221u) + 1376312589u;
    return (float)(n & 0x7fffffffu) / 2147483648.0f;
}

/* Smooth value noise round a circle: `t` in turns, `n` cells a turn, so it
   meets itself at the join. 0..1. */
static float ring_noise(float t, int n, uint32_t seed)
{
    float x = t * (float)n;
    float f0 = floorf(x);
    float f = x - f0;
    int i = wrapi((int)f0, n);
    float a = hash1((uint32_t)i * 7919u + seed);
    float b = hash1((uint32_t)wrapi(i + 1, n) * 7919u + seed);
    f = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * f;
}

/* Smooth value noise on the plane, 0..1, and three octaves of it. */
static float plane_noise(float x, float y, uint32_t seed)
{
    float fx = floorf(x), fy = floorf(y);
    float tx = x - fx, ty = y - fy;
    uint32_t ix = (uint32_t)(int32_t)fx, iy = (uint32_t)(int32_t)fy;
    float a = hash1(ix * 73856093u ^ iy * 19349663u ^ seed);
    float b = hash1((ix + 1u) * 73856093u ^ iy * 19349663u ^ seed);
    float c = hash1(ix * 73856093u ^ (iy + 1u) * 19349663u ^ seed);
    float d = hash1((ix + 1u) * 73856093u ^ (iy + 1u) * 19349663u ^ seed);
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    return mixf(mixf(a, b, tx), mixf(c, d, tx), ty);
}

static float fbm(float x, float y, uint32_t seed)
{
    return 0.55f * plane_noise(x, y, seed)
         + 0.30f * plane_noise(2.1f * x + 5.3f, 2.1f * y + 1.7f, seed + 1u)
         + 0.15f * plane_noise(4.3f * x + 9.1f, 4.3f * y + 3.3f, seed + 2u);
}

/* ---- the dial, pixel by pixel ------------------------------------------ */

/* A 4x4 Bayer matrix, for the ordered dither into 565. */
static const uint8_t s_bayer[16] = {
    0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5,
};

static uint16_t dither565(float r, float g, float b, int i, int j)
{
    float t = ((float)s_bayer[((j & 3) << 2) | (i & 3)] + 0.5f) / 16.0f;
    int r5 = (int)(clamp01(r / 255.0f) * 31.0f + t);
    int g6 = (int)(clamp01(g / 255.0f) * 63.0f + t);
    int b5 = (int)(clamp01(b / 255.0f) * 31.0f + t);
    if (r5 > 31) r5 = 31;
    if (g6 > 63) g6 = 63;
    if (b5 > 31) b5 = 31;
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* ---- photographs ---------------------------------------------------------
 * A dial photo or the moon texture: FACE_PHOTO_W x FACE_PHOTO_H RGB565. */

static void unpack565(uint16_t p, float out[3])
{
    out[0] = (float)((p >> 11) & 0x1F) * (255.0f / 31.0f);
    out[1] = (float)((p >> 5) & 0x3F) * (255.0f / 63.0f);
    out[2] = (float)(p & 0x1F) * (255.0f / 31.0f);
}

/* A dial photo's tone curve: each channel to the power PHOTO_GAMMA, which
   deepens the middle tones and leaves black and white where they are, so
   the moon's maria and the nebula's dust lanes keep their shape once the
   photo is dimmed. It is a table on the 5- and 6-bit texels rather than a
   powf per bilinear tap; filled on first use, and only for dial photos:
   the moon page's texture is sampled linear. */
#define PHOTO_GAMMA    1.3f

static float s_tone5[32], s_tone6[64];
static bool s_tone_ready;

static void tone_tables(void)
{
    if (s_tone_ready) return;
    for (int i = 0; i < 32; i++) s_tone5[i] = 255.0f * powf((float)i / 31.0f, PHOTO_GAMMA);
    for (int i = 0; i < 64; i++) s_tone6[i] = 255.0f * powf((float)i / 63.0f, PHOTO_GAMMA);
    s_tone_ready = true;
}

/* The photo at (fx, fy) in its own pixels, pixel (i, j)'s centre at
   (i, j), bilinear, 0..255 per channel; through the tone curve if `toned`
   (call tone_tables first). Beyond the photo it is black, so a panel of
   another shape sees the photo scaled with the dial and black round it
   rather than its edge rows smeared outwards. */
static void photo_sample(const uint16_t *img, float fx, float fy, bool toned,
                         float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!(fx > -1.0f && fx < (float)FACE_PHOTO_W && fy > -1.0f
          && fy < (float)FACE_PHOTO_H))
        return;                                 /* NaN lands here too */
    float x0f = floorf(fx), y0f = floorf(fy);
    int x0 = (int)x0f, y0 = (int)y0f;
    float tx = fx - x0f, ty = fy - y0f;
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        float wgt = ((k & 1) ? tx : 1.0f - tx) * ((k >> 1) ? ty : 1.0f - ty);
        if (wgt <= 0.0f || x < 0 || y < 0 || x >= FACE_PHOTO_W || y >= FACE_PHOTO_H)
            continue;
        float c[3];
        uint16_t p = img[y * FACE_PHOTO_W + x];
        if (toned) {
            c[0] = s_tone5[(p >> 11) & 0x1F];
            c[1] = s_tone6[(p >> 5) & 0x3F];
            c[2] = s_tone5[p & 0x1F];
        } else {
            unpack565(p, c);
        }
        for (int q = 0; q < 3; q++) out[q] += wgt * c[q];
    }
}

/*
 * A photo as the dial. It is set back rather than shown: toned (above),
 * dimmed to about half and a little desaturated, and then its highlights
 * rolled off, so that nothing in it comes up to the hands' shaded facets
 * and the gold and cream stay the brightest, warmest things on the face
 * whatever is behind them. Then four things that make it a dial rather
 * than a picture behind a dial:
 *
 *  - the corners fall away into shadow, as a dial does under its flange,
 *    so the panel's rounded corners close round it;
 *  - the minute track is a smoked chapter ring, a band darker than the
 *    photo either side of it, so the ticks read over cloud and nebula;
 *  - past the track the photo all but stops, so a photo that runs to the
 *    panel's edge (the nebula) still ends at the chapter ring as the
 *    Earth and the Moon do, rather than running on under it;
 *  - the power reserve's arch gets a smoked bed as the track does.
 *
 * The sub-dials and the moon aperture go over this as they do over the
 * sunburst. (ux, uy) in design units; out is 0..255 per channel.
 */
#define PHOTO_DIM      0.54f    /* the photo's brightness on the dial */
#define PHOTO_DESAT    0.18f    /* how far towards grey */
#define PHOTO_KNEE    64.0f     /* luminance past which its rise is halved */
#define SUB_SMOKE      0.82f    /* a sub-dial's opacity over the photo */
#define SUB_GREY       0.55f    /* and how far its navy goes to grey */

static void photo_ground(const uint16_t *photo, float ux, float uy, float out[3])
{
    tone_tables();
    photo_sample(photo, ux + 0.5f * (float)FACE_PHOTO_W - 0.5f,
                 uy + 0.5f * (float)FACE_PHOTO_H - 0.5f, true, out);
    float lum = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
    for (int k = 0; k < 3; k++) out[k] = mixf(out[k], lum, PHOTO_DESAT) * PHOTO_DIM;
    /* The roll-off: cloud tops, the Moon's rays and the brightest nebula
       would otherwise sit at the luminance of a hand's shaded facet and
       leave only its outline to tell them apart. Scaled as a whole, so
       the colour stays as it was. */
    float y = 0.2126f * out[0] + 0.7152f * out[1] + 0.0722f * out[2];
    if (y > PHOTO_KNEE) {
        float k = (PHOTO_KNEE + 0.5f * (y - PHOTO_KNEE)) / y;
        for (int q = 0; q < 3; q++) out[q] *= k;
    }

    float r = sqrtf(ux * ux + uy * uy);
    float keep = 1.0f - 0.62f * smooth(104.0f, 186.0f, r);
    float ring = smooth(TRACK_IN - 5.0f, TRACK_IN - 1.0f, r)
               * (1.0f - smooth(TRACK_OUT + 1.0f, TRACK_OUT + 5.0f, r));
    keep *= 1.0f - 0.50f * ring;
    keep *= 1.0f - 0.85f * smooth(TRACK_OUT + 0.5f, TRACK_OUT + 6.0f, r);

    float mx = ux, my = uy - MOON_Y;
    float rr = sqrtf(mx * mx + my * my);
    float off = fabsf(rr - RES_R);
    if (off < 6.5f && my < 8.0f) {
        float a = fabsf(atan2f(mx, -my));
        float bed = (1.0f - smooth(2.4f, 6.0f, off))
                  * (1.0f - smooth(RES_HALF + 1.0f * DEG, RES_HALF + 9.0f * DEG, a));
        keep *= 1.0f - 0.55f * bed;
    }
    for (int k = 0; k < 3; k++) out[k] *= keep;
}

/*
 * The sunburst: fine brushing running out from the centre, which catches
 * the light in a broad bow-tie along the light's axis and leaves the cross
 * axis dark. The brushing is noise in angle only, so every streak is a
 * straight ray; it fades near the centre, where the rays converge finer than
 * a pixel and would only sparkle. The edges and corners fall away darker, as
 * a dial does under its flange.
 */
static void sunburst(float ux, float uy, float out[3])
{
    float r = sqrtf(ux * ux + uy * uy);
    float th = atan2f(uy, ux);
    float d = th - LIGHT_ANGLE;
    float bow = 0.5f + 0.5f * cosf(2.0f * d);
    /* The near side a little brighter, but only a little: any more and the
       far lobe fades out and the bow-tie reads as a searchlight. */
    bow = bow * bow * (0.80f + 0.20f * cosf(d));

    float t = th / TWO_PI + 0.5f;
    float streak = 0.62f * ring_noise(t, 331, 17u) + 0.28f * ring_noise(t, 757, 91u)
                 + 0.10f * ring_noise(t, 1409, 5u) - 0.5f;
    streak *= smooth(6.0f, 60.0f, r);

    float glow = 1.0f - 0.55f * smooth(60.0f, 175.0f, r);
    float lit = clamp01((0.10f + 0.90f * bow) * glow + streak * (0.07f + 0.22f * bow));

    static const float dark[3] = { 3.0f, 5.0f, 15.0f };
    static const float light[3] = { 30.0f, 48.0f, 94.0f };
    for (int k = 0; k < 3; k++) out[k] = mixf(dark[k], light[k], lit);
}

/*
 * A sub-dial: sunk a little below the dial and snailed, cut with fine
 * concentric grooves, so it reads as a separate recessed surface. The
 * grooves reflect along the axis across the light rather than along it,
 * which is what sets them apart from the sunburst at a glance. The wall of
 * the recess is dark on the side towards the light and caught on the far
 * side.
 */
static void snailed(const geo_t *g, float lx, float ly, bool grooves,
                    float out[3])
{
    float r = sqrtf(lx * lx + ly * ly);
    float th = atan2f(ly, lx);
    float d = th - LIGHT_ANGLE;
    float bow = 0.5f + 0.5f * cosf(2.0f * d + PI);
    bow = bow * bow;
    /* Three pixels a groove, whatever the scale: any finer and the rings
       alias on the pixel grid into a checker instead of reading as rings. */
    float groove = grooves ? cosf(TWO_PI * r * g->s / 3.0f) : 0.0f;
    float lit = 0.14f + 0.36f * bow + (0.05f + 0.07f * bow) * groove;

    /* The wall: the outer 1.6 units, dark towards the light. */
    float wall = smooth(SUB_R - 1.8f, SUB_R, r);
    lit += wall * (-0.18f * cosf(d) - 0.04f);
    lit = clamp01(lit);

    static const float dark[3] = { 2.0f, 5.0f, 15.0f };
    static const float light[3] = { 32.0f, 50.0f, 96.0f };
    for (int k = 0; k < 3; k++) out[k] = mixf(dark[k], light[k], lit);
}

/* The sub-dial's ground at (lx, ly) from its centre, the grooves averaged
   out, as one colour. A label a hand is crossing is set again over a
   knockout in this, a stroke wider than the label, which cuts the hand
   back from the letters: the hand then reads as passing under the
   lettering, where a fixed dark knockout would leave a visible outline on
   the lighter side of the dial. */
static uint16_t sub_ground(const geo_t *g, float lx, float ly)
{
    float col[3];
    snailed(g, lx, ly, false, col);
    int r5 = (int)(clamp01(col[0] / 255.0f) * 31.0f + 0.5f);
    int g6 = (int)(clamp01(col[1] / 255.0f) * 63.0f + 0.5f);
    int b5 = (int)(clamp01(col[2] / 255.0f) * 31.0f + 0.5f);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* Stars on the moon disc, fixed to it and turning with it: angle in turns
   from the first moon, radius in units, size in units. Placed by hand to
   keep clear of both moons' paths. */
typedef struct { float t, r, size; } star_t;
static const star_t s_stars[] = {
    { 0.100f, 26.5f, 0.95f }, { 0.140f, 11.0f, 0.60f }, { 0.175f, 20.0f, 0.75f },
    { 0.205f, 28.5f, 0.55f }, { 0.240f, 14.5f, 1.05f }, { 0.270f, 24.0f, 0.60f },
    { 0.310f, 29.5f, 0.80f }, { 0.330f, 9.0f, 0.55f },  { 0.360f, 19.0f, 0.70f },
    { 0.400f, 26.0f, 0.95f }, { 0.425f, 13.0f, 0.60f },
    { 0.600f, 27.0f, 0.90f }, { 0.635f, 12.0f, 0.60f }, { 0.660f, 21.5f, 0.75f },
    { 0.700f, 29.0f, 0.60f }, { 0.740f, 15.5f, 1.05f }, { 0.770f, 25.0f, 0.55f },
    { 0.805f, 29.5f, 0.85f }, { 0.830f, 8.5f, 0.55f },  { 0.860f, 18.5f, 0.75f },
    { 0.895f, 26.0f, 0.95f }, { 0.925f, 12.0f, 0.55f },
    { 0.050f, 29.5f, 0.55f }, { 0.950f, 29.0f, 0.55f },
    { 0.445f, 28.5f, 0.60f }, { 0.555f, 28.5f, 0.60f },
};
#define STAR_COUNT ((int)(sizeof s_stars / sizeof s_stars[0]))

/* The window's signed distance in units, negative inside: inside the half
   disc above the base and outside both humps. */
static float window_sdf(float mx, float my)
{
    float top = sqrtf(mx * mx + my * my) - MOON_RA;
    if (my > top) top = my;
    float hx = mx + MOON_RP, h1 = MOON_RH - sqrtf(hx * hx + my * my);
    hx = mx - MOON_RP;
    float h2 = MOON_RH - sqrtf(hx * hx + my * my);
    if (h1 > top) top = h1;
    if (h2 > top) top = h2;
    return top;
}

/* One moon at (0, 0) in its own frame, radius MOON_RM: pale gold, a little
   brighter towards the light, with the maria engraved faintly into it. The
   frame turns with the disc, so the markings do too. */
static void moon_gold(float x, float y, float out[3])
{
    float nx = x / MOON_RM, ny = y / MOON_RM;
    float lit = 0.80f + 0.20f * clamp01(nx * s_light_x + ny * s_light_y + 0.3f);
    static const float mare[][3] = {
        { -0.30f, -0.38f, 0.28f }, { 0.14f, -0.36f, 0.18f }, { 0.28f, -0.08f, 0.20f },
        { 0.62f, -0.22f, 0.11f }, { -0.52f, 0.02f, 0.26f }, { -0.14f, 0.36f, 0.17f },
        { 0.44f, 0.22f, 0.13f },
    };
    float m = 0.0f;
    for (int k = 0; k < (int)(sizeof mare / sizeof mare[0]); k++) {
        float dx = nx - mare[k][0], dy = ny - mare[k][1];
        float q = 1.0f - (dx * dx + dy * dy) / (mare[k][2] * mare[k][2]);
        if (q > 0.0f) m += q * q;
    }
    lit *= 1.0f - 0.16f * clamp01(m);
    out[0] = 238.0f * lit;
    out[1] = 212.0f * lit;
    out[2] = 146.0f * lit;
}

/*
 * What shows through the aperture at (mx, my), units from the pivot: the
 * disc, lapis blue with gold stars, and the two moons, turned by `alpha`
 * (the first moon's angle from straight up, clockwise).
 */
static void moon_disc(const geo_t *g, float mx, float my, float ca, float sa,
                      const float (*star_xy)[2], float out[3])
{
    /* Into the disc's own frame, where the first moon is straight up. */
    float dx = mx * ca + my * sa;
    float dy = -mx * sa + my * ca;
    float r = sqrtf(mx * mx + my * my);

    float deep = smooth(4.0f, MOON_RA + 2.0f, r);
    out[0] = mixf(30.0f, 12.0f, deep);
    out[1] = mixf(58.0f, 26.0f, deep);
    out[2] = mixf(142.0f, 78.0f, deep);

    for (int k = 0; k < STAR_COUNT; k++) {
        float ex = dx - star_xy[k][0], ey = dy - star_xy[k][1];
        float e2 = ex * ex + ey * ey;
        float reach = s_stars[k].size + 1.5f;
        if (e2 > reach * reach) continue;
        float cov = clamp01((s_stars[k].size - sqrtf(e2)) * g->s + 0.5f);
        out[0] = mixf(out[0], 250.0f, cov);
        out[1] = mixf(out[1], 226.0f, cov);
        out[2] = mixf(out[2], 160.0f, cov);
    }

    for (int m = 0; m < 2; m++) {
        float cy = m == 0 ? -MOON_RP : MOON_RP;
        float ex = dx, ey = dy - cy;
        float e = sqrtf(ex * ex + ey * ey);
        if (e > MOON_RM + 1.5f) continue;
        float cov = clamp01((MOON_RM - e) * g->s + 0.5f);
        float mc[3];
        /* The second moon is the first turned half a revolution. */
        if (m == 0) moon_gold(ex, ey, mc);
        else moon_gold(-ex, -ey, mc);
        for (int k = 0; k < 3; k++) out[k] = mixf(out[k], mc[k], cov);
    }
}

/* The disc's turn for a moon phase, in whole degrees: 0 at new, 90 at
   full. Two moons half a turn apart make the picture repeat every 180. */
static int moon_step_for(double phase)
{
    double p = phase - floor(phase);
    if (!(p >= 0.0 && p < 1.0)) p = 0.0;
    return (int)floor(p * 180.0 + 0.5) % 180;
}

static const float s_sub_x[3] = { -SUB_D, SUB_D, 0.0f };
static const float s_sub_y[3] = { 0.0f, 0.0f, SUB_D };

/* The dial at (ux, uy) in design units, short of the moon aperture: the
   sunburst or the photo, and the sub-dials over it -- opaque on the
   sunburst, smoked glass over a photo. `reach` is two pixels in units. */
static void dial_pixel(const geo_t *g, const uint16_t *photo, float ux, float uy,
                       bool grooves, float reach, float col[3])
{
    if (photo != NULL) photo_ground(photo, ux, uy, col);
    else sunburst(ux, uy, col);

    for (int k = 0; k < 3; k++) {
        float lx = ux - s_sub_x[k], ly = uy - s_sub_y[k];
        float r2 = lx * lx + ly * ly;
        if (r2 > (SUB_R + reach) * (SUB_R + reach)) continue;
        float cov = clamp01((SUB_R - sqrtf(r2)) * g->s + 0.5f);
        float sc[3];
        snailed(g, lx, ly, grooves, sc);
        if (photo != NULL) {
            /* Smoked: the navy taken most of the way to a neutral
               charcoal, so the sub-dial sits in whatever colour the photo
               has there rather than reading as a blue patch on it. */
            float sl = 0.2126f * sc[0] + 0.7152f * sc[1] + 0.0722f * sc[2];
            for (int q = 0; q < 3; q++) sc[q] = mixf(sc[q], sl, SUB_GREY);
            cov *= SUB_SMOKE;
        }
        for (int q = 0; q < 3; q++) col[q] = mixf(col[q], sc[q], cov);
    }
}

static void shade_dial(canvas_t *c, const geo_t *g, int moon_step,
                       const uint16_t *photo)
{
    float alpha = (float)moon_step * DEG - 0.5f * PI;
    float ca = cosf(alpha), sa = sinf(alpha);
    /* Where the stars sit on the disc, worked out once rather than for
       every pixel of the aperture. */
    float star_xy[STAR_COUNT][2];
    for (int k = 0; k < STAR_COUNT; k++) {
        float a = s_stars[k].t * TWO_PI;
        star_xy[k][0] = s_stars[k].r * sinf(a);
        star_xy[k][1] = -s_stars[k].r * cosf(a);
    }
    float inv = 1.0f / g->s;
    float reach = 2.0f * inv;           /* two pixels, in units */

    for (int j = 0; j < c->h; j++) {
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        float uy = ((float)j + 0.5f - g->cy) * inv;
        for (int i = 0; i < c->w; i++) {
            float ux = ((float)i + 0.5f - g->cx) * inv;
            float col[3];
            dial_pixel(g, photo, ux, uy, true, reach, col);

            float mx = ux, my = uy - MOON_Y;
            if (mx > -MOON_RA - reach && mx < MOON_RA + reach
                && my > -MOON_RA - reach && my < reach) {
                float sdf = window_sdf(mx, my) * g->s;    /* pixels */
                if (sdf < 1.5f) {
                    float cov = clamp01(0.5f - sdf);
                    if (cov > 0.0f) {
                        float mc[3];
                        moon_disc(g, mx, my, ca, sa, (const float (*)[2])star_xy, mc);
                        /* The dial's edge shades the disc just inside it. */
                        float shade = 1.0f - 0.55f * (1.0f - smooth(0.0f, 3.0f * g->s, -sdf));
                        for (int q = 0; q < 3; q++) col[q] = mixf(col[q], mc[q] * shade, cov);
                    }
                    /* A fine bronze edge round the aperture, the same as
                       the sub-dials' rings; gilt here would double the
                       power reserve's arch and draw the eye off the moon. */
                    float e = fabsf(sdf + 0.1f);
                    float hw = 0.45f * g->s;
                    float lc = clamp01((hw < 0.5f ? hw : 0.5f) + (hw - e < 0.5f ? hw - e : 0.5f));
                    if (lc > 0.0f)
                        for (int q = 0; q < 3; q++)
                            col[q] = mixf(col[q], s_bronze[q], 0.9f * lc);
                }
            }
            row[i] = dither565(col[0], col[1], col[2], i, j);
        }
    }
}

/* ---- the printed and applied parts ------------------------------------- */

/* A fine dark outline round a polygon: the outline drawn `o` pixels out in
   eight directions, under the polygon itself. Over the sunburst the dial is
   dark enough that the drop shadows alone do for most things; over bright
   cloud or nebula the gold and cream need an edge of their own. The two
   leaf hands take a wider one over a photo, as they are the things that
   must read at a glance and they lie across the photo's brightest parts;
   on navy the outline is all but invisible except where a hand crosses
   something light, the gold moons or the reserve, and there it is the edge
   the drop shadow alone cannot give on the side towards the light. */
#define HALO_PX      0.9f
#define HALO_HAND_PX 1.3f
#define HALO         RGB(4, 6, 12)

static void poly_halo(canvas_t *c, const float *xy, int n, float o, uint16_t colour)
{
    static const float dir[8][2] = {
        { 1.0f, 0.0f }, { 0.7071f, 0.7071f }, { 0.0f, 1.0f }, { -0.7071f, 0.7071f },
        { -1.0f, 0.0f }, { -0.7071f, -0.7071f }, { 0.0f, -1.0f }, { 0.7071f, -0.7071f },
    };
    float tmp[2 * VEC_POLY_MAX];
    if (n < 3 || n > VEC_POLY_MAX) return;
    for (int d = 0; d < 8; d++) {
        for (int k = 0; k < n; k++) {
            tmp[2 * k] = xy[2 * k] + o * dir[d][0];
            tmp[2 * k + 1] = xy[2 * k + 1] + o * dir[d][1];
        }
        vec_polygon(c, tmp, n, colour);
    }
}

static void minute_track(canvas_t *c, const geo_t *g)
{
    float w = 0.65f * g->s;
    vec_ring(c, g->cx, g->cy, TRACK_OUT * g->s, w, RAIL);
    vec_ring(c, g->cx, g->cy, TRACK_IN * g->s, w, RAIL);
    for (int m = 0; m < 60; m++) {
        float a = (float)m * 6.0f * DEG;
        float x0, y0, x1, y1;
        bool five = m % 5 == 0;
        polar(g, 0, 0, TRACK_IN, a, &x0, &y0);
        polar(g, 0, 0, TRACK_OUT, a, &x1, &y1);
        vec_line(c, x0, y0, x1, y1, (five ? 1.25f : 0.75f) * g->s,
                 five ? CREAM_SOFT : TICK);
    }
}

/* One applied baton: a slim wedge, wider at the rim, split along its length
   into two facets that take the light differently. `off` shifts it across
   its own axis, for the pair at 12, and `side` then gives the whole baton
   one facet's shade (-1 the anticlockwise one, +1 the clockwise), so the
   pair reads as one wide faceted index split down its ridge rather than as
   two separate bars. */
static void baton(canvas_t *c, const geo_t *g, float a, float off, int side,
                  bool halo)
{
    float ex = sinf(a), ey = -cosf(a);          /* outward */
    float nx = cosf(a), ny = sinf(a);           /* across, clockwise side */
    /* Half-widths, inner and outer: each facet over a pixel wide even at
       the inner end, or at 1x the faceting cannot show at all. */
    float wi = 1.45f, wo = 2.1f;
    float pts[4][2] = {
        { BATON_IN, -wi }, { BATON_OUT, -wo }, { BATON_OUT, wo }, { BATON_IN, wi },
    };
    float xy[8], half[8];
    float sh = 0.9f * g->s;                     /* shadow offset, pixels */

    for (int k = 0; k < 4; k++) {
        float r = pts[k][0], v = pts[k][1] + off;
        xy[2 * k] = g->cx + (r * ex + v * nx) * g->s;
        xy[2 * k + 1] = g->cy + (r * ey + v * ny) * g->s;
    }
    float sxy[8];
    for (int k = 0; k < 4; k++) {
        sxy[2 * k] = xy[2 * k] + sh * -s_light_x;
        sxy[2 * k + 1] = xy[2 * k + 1] + sh * -s_light_y;
    }
    vec_polygon(c, sxy, 4, SHADOW);
    if (halo) poly_halo(c, xy, 4, HALO_PX, HALO);

    /* The facet across the light is the brighter one. */
    float facing = nx * s_light_x + ny * s_light_y;   /* +1: clockwise side lit */
    uint16_t lit = mix565(CREAM, RGB(255, 252, 240), 0.6f);
    uint16_t dim = mix565(CREAM, RGB(120, 116, 106), 0.55f);
    uint16_t ccw = facing > 0 ? dim : lit, cw = facing > 0 ? lit : dim;
    vec_polygon(c, xy, 4, side > 0 ? cw : ccw);
    if (side != 0) return;

    /* The clockwise half: from the inner middle out to the outer middle. */
    float mid_in_x = g->cx + (BATON_IN * ex + off * nx) * g->s;
    float mid_in_y = g->cy + (BATON_IN * ey + off * ny) * g->s;
    float mid_out_x = g->cx + (BATON_OUT * ex + off * nx) * g->s;
    float mid_out_y = g->cy + (BATON_OUT * ey + off * ny) * g->s;
    half[0] = mid_in_x;  half[1] = mid_in_y;
    half[2] = mid_out_x; half[3] = mid_out_y;
    half[4] = xy[4];     half[5] = xy[5];
    half[6] = xy[6];     half[7] = xy[7];
    vec_polygon(c, half, 4, cw);
}

static void batons(canvas_t *c, const geo_t *g, bool halo)
{
    for (int h = 0; h < 12; h++) {
        float a = (float)h * 30.0f * DEG;
        if (h == 0) {
            /* Both halos first, so neither draws over the other's baton. */
            if (halo) {
                baton(c, g, a, -2.55f, -1, true);
                baton(c, g, a, 2.55f, 1, true);
            }
            baton(c, g, a, -2.55f, -1, false);
            baton(c, g, a, 2.55f, 1, false);
        } else {
            baton(c, g, a, 0.0f, 0, halo);
        }
    }
}

/* Five and a half units: at five, even with vfont's wide M and deep U, MON
   and SUN are a pixel from reading HON and SLIN. */
static const vfont_style_t s_sub_font = { 5.5f, 1.0f, 0.25f };

/* A sub-dial's divisions run in from SUB_TICK_OUT, a short one to
   SUB_TICK_IN and a long one to SUB_MAJOR_IN. */
#define SUB_TICK_OUT (SUB_R - 1.5f)
#define SUB_TICK_IN  (SUB_R - 3.2f)
#define SUB_MAJOR_IN (SUB_R - 4.6f)

/* The clear space, in units, between a division and the label it marks. */
#define LABEL_GAP     2.6f

static vfont_style_t scaled(const vfont_style_t *st, const geo_t *g)
{
    vfont_style_t out = { st->size * g->s, st->weight * g->s, st->tracking * g->s };
    return out;
}

/* Every `step`th of `n` divisions from `first`, as a bit mask. */
static uint64_t every(int n, int step, int first)
{
    uint64_t m = 0;
    for (int k = first; k < n && k < 64; k += step) m |= (uint64_t)1 << k;
    return m;
}

/* A sub-dial's ring and scale: `n` divisions, those whose bit is set in
   `majors` longer and brighter. The ring is bronze, as the aperture's
   edge is, rather than the hands' gold, which is kept for what moves.
   Over a photo the navy is no longer there to frame it, and a bronze of
   the photo's own brightness vanishes into cloud or nebula, so there the
   ring is a lighter bronze, a little heavier, between two fine dark lines:
   it then reads against light ground and dark alike. */
#define SUB_RING_PHOTO RGB(150, 122, 84)

static void sub_scale(canvas_t *c, const geo_t *g, float ox, float oy, int n,
                      uint64_t majors, bool photo)
{
    if (photo) {
        /* A fine dark line inside the bronze, against the smoked disc,
           and a heavier one outside it, against the photo. */
        vec_ring(c, px(g, ox), py(g, oy), (SUB_R + 0.3f) * g->s,
                 0.8f * g->s + 1.6f, HALO);
        vec_ring(c, px(g, ox), py(g, oy), SUB_R * g->s, 1.0f * g->s, SUB_RING_PHOTO);
    } else {
        vec_ring(c, px(g, ox), py(g, oy), SUB_R * g->s, 0.8f * g->s, BRONZE);
    }
    for (int k = 0; k < n; k++) {
        float a = (float)k * TWO_PI / (float)n;
        bool big = k < 64 && ((majors >> k) & 1u);
        float x0, y0, x1, y1;
        polar(g, ox, oy, big ? SUB_MAJOR_IN : SUB_TICK_IN, a, &x0, &y0);
        polar(g, ox, oy, SUB_TICK_OUT, a, &x1, &y1);
        vec_line(c, x0, y0, x1, y1, (big ? 0.95f : 0.6f) * g->s,
                 big ? CREAM : CREAM_SOFT);
    }
}

static const char *const s_days[7] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT",
};

static float day_angle(int wd) { return (float)wd * TWO_PI / 7.0f; }
static float date_angle(int d) { return (float)(d - 1) * TWO_PI / 31.0f; }

/*
 * Where a label's centre goes, in units from its sub-dial's pivot: in from
 * the end of its division by LABEL_GAP plus the label's own reach along
 * that division's ray, which is where the ray leaves the box round its ink.
 * So every label stands the same distance clear of its division, the way
 * numerals are set inside a chapter ring: a wide label at 3 or 9, whose
 * reach is half its width, sits further in than a short one at 12, whose
 * reach is half its height. On one radius, TUE or 25 would all but touch
 * their divisions while SUN stood well clear of its own.
 */
static float label_radius(const geo_t *g, const vfont_style_t *f, float a,
                          float inner, const char *text)
{
    float hw = 0.5f * (vfont_width(f, text) + f->weight) / g->s;
    float hh = 0.5f * (f->size + f->weight) / g->s;
    float sa = fabsf(sinf(a)), ca = fabsf(cosf(a));
    float reach = hh;
    if (sa * hh > ca * hw) reach = hw / sa;         /* leaves by a side */
    else if (ca > 0.0f) reach = hh / ca;            /* by the top or foot */
    return inner - LABEL_GAP - reach;
}

static void sub_label(canvas_t *c, const geo_t *g, const vfont_style_t *f,
                      float ox, float oy, float a, float r, uint16_t colour,
                      const char *text)
{
    float x, y;
    polar(g, ox, oy, r, a, &x, &y);
    vfont_draw(c, f, x, y, 0.0f, VFONT_CENTRE, colour, text);
}

static void subdials(canvas_t *c, const geo_t *g, bool photo)
{
    vfont_style_t f = scaled(&s_sub_font, g);
    char buf[8];

    /* Day of the week, at 9. All seven are printed dim; today's is set
       again in gold with the hands, so it is the one the eye lands on. */
    sub_scale(c, g, -SUB_D, 0.0f, 7, 0, photo);
    for (int k = 0; k < 7; k++) {
        float a = day_angle(k);
        sub_label(c, g, &f, -SUB_D, 0.0f, a,
                  label_radius(g, &f, a, SUB_TICK_IN, s_days[k]), CREAM_DIM,
                  s_days[k]);
    }

    /* Date, at 3: a division for every day, numerals every fifth, and a
       long division for each numeral and for the first of the month, which
       gets no numeral of its own since it would crowd the 30 beside it. */
    sub_scale(c, g, SUB_D, 0.0f, 31, (uint64_t)1 | every(31, 5, 4), photo);
    for (int d = 5; d <= 30; d += 5) {
        float a = date_angle(d);
        snprintf(buf, sizeof buf, "%d", d);
        sub_label(c, g, &f, SUB_D, 0.0f, a,
                  label_radius(g, &f, a, SUB_MAJOR_IN, buf), CREAM, buf);
    }

    /* Small seconds, at 6. */
    sub_scale(c, g, 0.0f, SUB_D, 60, every(60, 5, 0), photo);
    static const char *const secs[4] = { "60", "15", "30", "45" };
    for (int k = 0; k < 4; k++) {
        float a = (float)k * 0.5f * PI;
        sub_label(c, g, &f, 0.0f, SUB_D, a,
                  label_radius(g, &f, a, SUB_MAJOR_IN, secs[k]), CREAM, secs[k]);
    }
}

static void shade_static(canvas_t *c, int moon_step, const uint16_t *photo)
{
    geo_t g = geo_for(c->w, c->h);
    shade_dial(c, &g, moon_step, photo);
    minute_track(c, &g);
    batons(c, &g, photo != NULL);
    subdials(c, &g, photo != NULL);
}

/* ---- the moving parts --------------------------------------------------- */

/*
 * The power reserve, drawn with the hands rather than into the background,
 * so a new battery reading never costs a rebuild. Four quarters of a track;
 * the first sixth tinted red; the level filled in champagne (red once it is
 * in the red zone) with a bright index at its end.
 *
 * With no reading the track is left bare, grey and with no red zone, which
 * cannot be mistaken for an empty battery, since empty shows the red zone
 * and the index at zero. Either way a cream mark closes each end of the
 * scale, so a full reserve and a track that merely stops read differently.
 * Charging adds a bolt above the arch, midway between it and the 12.
 */
static float reserve_angle(float frac)
{
    return -RES_HALF + 2.0f * RES_HALF * frac;
}

/* The track between two fractions, cut at the gaps between quarters. */
static void reserve_run(canvas_t *c, const geo_t *g, float f0, float f1,
                        uint16_t colour)
{
    float ox = px(g, 0.0f), oy = py(g, MOON_Y);
    for (int q = 0; q < 4; q++) {
        float a0 = reserve_angle(0.25f * (float)q) + (q > 0 ? RES_GAP : 0.0f);
        float a1 = reserve_angle(0.25f * (float)(q + 1)) - (q < 3 ? RES_GAP : 0.0f);
        float b0 = reserve_angle(f0), b1 = reserve_angle(f1);
        if (b0 > a0) a0 = b0;
        if (b1 < a1) a1 = b1;
        if (a1 > a0)
            vec_arc_butt(c, ox, oy, RES_R * g->s, RES_W * g->s, a0, a1, colour);
    }
}

/* A radial mark across the track at `frac`, reaching `reach` either side
   of it. */
static void reserve_mark(canvas_t *c, const geo_t *g, float frac, float reach,
                         float width, uint16_t colour)
{
    float x0, y0, x1, y1;
    polar(g, 0.0f, MOON_Y, RES_R - reach, reserve_angle(frac), &x0, &y0);
    polar(g, 0.0f, MOON_Y, RES_R + reach, reserve_angle(frac), &x1, &y1);
    vec_line(c, x0, y0, x1, y1, width * g->s, colour);
}

static void reserve(canvas_t *c, const geo_t *g, int pct, bool charging,
                    bool halo)
{
    if (pct < 0) {
        reserve_run(c, g, 0.0f, 1.0f, RGB(64, 72, 92));
        reserve_mark(c, g, 0.0f, 2.2f, 1.0f, CREAM);
        reserve_mark(c, g, 1.0f, 2.2f, 1.0f, CREAM);
    } else {
        if (pct > 100) pct = 100;
        float frac = (float)pct / 100.0f;
        bool low = frac <= RES_LOW;
        reserve_run(c, g, 0.0f, 1.0f, RESERVE_BG);
        reserve_run(c, g, 0.0f, RES_LOW, mix565(RESERVE_BG, RESERVE_RED, 0.45f));
        reserve_run(c, g, 0.0f, frac, low ? RESERVE_RED : mix565(GOLD, CREAM, 0.45f));
        reserve_mark(c, g, 0.0f, 2.2f, 1.0f, CREAM);
        reserve_mark(c, g, 1.0f, 2.2f, 1.0f, CREAM);
        reserve_mark(c, g, frac, 2.3f, 1.0f,
                     low ? mix565(RESERVE_RED, CREAM, 0.35f) : GOLD_HI);
    }
    if (charging) {
        float x = px(g, 0.0f), y = py(g, MOON_Y - RES_R - 11.7f);
        float s = 1.6f * g->s;
        float bolt[12] = {
            x + 0.9f * s, y - 3.4f * s,  x - 1.7f * s, y + 0.55f * s,
            x - 0.1f * s, y + 0.55f * s, x - 0.9f * s, y + 3.4f * s,
            x + 1.7f * s, y - 0.55f * s, x + 0.1f * s, y - 0.55f * s,
        };
        if (halo) poly_halo(c, bolt, 6, HALO_PX, HALO);
        vec_polygon(c, bolt, 6, mix565(GOLD, CREAM, 0.45f));
    }
}

/* A sub-dial hand: a fine gold needle from a small counterweight, on its
   own pivot. The needle is a little over a pixel wide, or at 1x it is a
   hairline whose gold cannot be told from the cream print. Over a photo it
   gets a fine dark outline, as the batons do. */
static void sub_hand(canvas_t *c, const geo_t *g, float ox, float oy, float a,
                     float len, bool halo)
{
    float x0, y0, x1, y1, xt, yt;
    polar(g, ox, oy, len, a, &x1, &y1);
    polar(g, ox, oy, -4.0f, a, &xt, &yt);
    polar(g, ox, oy, 0.0f, a, &x0, &y0);
    float sh = 0.8f * g->s;
    vec_line(c, xt + sh * 0.6f, yt + sh * 0.8f, x1 + sh * 0.6f, y1 + sh * 0.8f,
             1.5f * g->s, SHADOW);
    if (halo) {
        vec_line(c, xt, yt, x1, y1, 1.4f * g->s + 2.0f * HALO_PX, HALO);
        vec_disc(c, x0, y0, 2.1f * g->s + HALO_PX, HALO);
    }
    vec_line(c, xt, yt, x1, y1, 1.4f * g->s, GOLD);
    vec_disc(c, xt, yt, 1.5f * g->s, GOLD);
    vec_disc(c, x0, y0, 2.1f * g->s, GOLD_LO);
    vec_disc(c, x0, y0, 1.6f * g->s, GOLD);
    vec_disc(c, x0, y0, 0.55f * g->s, GOLD_DEEP);
}

/*
 * A leaf hand, faceted like a dauphine: the outline rises from a slim neck
 * to its widest a little under halfway and closes to a sharp point, and a
 * ridge down the middle splits it into two facets. Each facet's shade is
 * worked from which way it faces the light, smoothly, so the hand does not
 * flicker from light to dark as it passes the light's axis.
 *
 * It is outlined on either dial (see poly_halo), more heavily over a photo,
 * and over a photo its shaded facet is kept well above the photo's
 * brightest ground, which the roll-off in photo_ground holds down, so the
 * hand is told from the ground by its own colour and not by the outline
 * alone.
 */
static void leaf_hand(canvas_t *c, const geo_t *g, float a, float len,
                      float wmax, float neck, float tail, bool photo)
{
#define LEAF_N 14
    float ex = sinf(a), ey = -cosf(a);
    float nx = cosf(a), ny = sinf(a);
    float u0 = 0.16f * len;                     /* where the leaf starts */
    float side[LEAF_N + 1][2];                  /* (u, v) along one edge */
    int n = 0;
    side[n][0] = -tail;       side[n][1] = neck * 0.7f;  n++;
    side[n][0] = u0 * 0.5f;   side[n][1] = neck;         n++;
    for (int k = 1; k < LEAF_N - 1; k++) {
        float t = (float)k / (float)(LEAF_N - 1);
        float w = wmax * powf(sinf(PI * powf(t, 0.78f)), 0.85f);
        side[n][0] = u0 + (len - u0) * t;
        side[n][1] = w > neck ? w : neck;
        n++;
    }
    /* n points on one side, then the tip. */

    /* Both edges and the tip; one edge, the tip and the tail's middle. */
    float full[2 * (2 * LEAF_N + 2)], half[2 * (LEAF_N + 3)];
    int fn = 0, hn = 0;
    /* Clockwise side out to the tip, the other back. */
    for (int k = 0; k < n; k++) {
        full[2 * fn] = side[k][0] * ex + side[k][1] * nx;
        full[2 * fn + 1] = side[k][0] * ey + side[k][1] * ny;
        fn++;
    }
    full[2 * fn] = len * ex;
    full[2 * fn + 1] = len * ey;
    fn++;
    for (int k = n - 1; k >= 0; k--) {
        full[2 * fn] = side[k][0] * ex - side[k][1] * nx;
        full[2 * fn + 1] = side[k][0] * ey - side[k][1] * ny;
        fn++;
    }
    /* The clockwise facet: the tail's middle, that edge, the tip. */
    half[0] = -tail * ex;
    half[1] = -tail * ey;
    hn = 1;
    for (int k = 0; k < n; k++) {
        half[2 * hn] = full[2 * k];
        half[2 * hn + 1] = full[2 * k + 1];
        hn++;
    }
    half[2 * hn] = len * ex;
    half[2 * hn + 1] = len * ey;
    hn++;

    float xy[2 * (2 * LEAF_N + 2)];
    float sh = 1.6f * g->s;
    for (int k = 0; k < fn; k++) {
        xy[2 * k] = g->cx + full[2 * k] * g->s - s_light_x * sh;
        xy[2 * k + 1] = g->cy + full[2 * k + 1] * g->s - s_light_y * sh;
    }
    vec_polygon(c, xy, fn, SHADOW);

    for (int k = 0; k < fn; k++) {
        xy[2 * k] = g->cx + full[2 * k] * g->s;
        xy[2 * k + 1] = g->cy + full[2 * k + 1] * g->s;
    }
    poly_halo(c, xy, fn, photo ? HALO_HAND_PX : HALO_PX, HALO);

    float facing = nx * s_light_x + ny * s_light_y;
    /* However the hand lies, one facet stays a step brighter than the
       other, as a real one does under a lamp that is also a little in
       front of it. */
    float lift = facing >= 0.0f ? 0.18f : -0.18f;
    float tcw = 0.5f + 0.32f * facing + lift, tccw = 0.5f - 0.32f * facing - lift;
    if (photo) {
        /* The shaded facet, 0 to 0.32 of the way to GOLD_HI, lifted to
           0.35 to 0.45: at GOLD_LO it is the luminance of cloud. */
        if (tcw < 0.5f) tcw = 0.35f + 0.3f * tcw;
        if (tccw < 0.5f) tccw = 0.35f + 0.3f * tccw;
    }
    uint16_t cw = mix565(GOLD_LO, GOLD_HI, tcw);
    uint16_t ccw = mix565(GOLD_LO, GOLD_HI, tccw);

    vec_polygon(c, xy, fn, ccw);
    for (int k = 0; k < hn; k++) {
        xy[2 * k] = g->cx + half[2 * k] * g->s;
        xy[2 * k + 1] = g->cy + half[2 * k + 1] * g->s;
    }
    vec_polygon(c, xy, hn, cw);

    /* The ridge, a fine darker line from the neck to near the tip. */
    vec_line(c, g->cx + u0 * 0.6f * ex * g->s, g->cy + u0 * 0.6f * ey * g->s,
             g->cx + len * 0.9f * ex * g->s, g->cy + len * 0.9f * ey * g->s,
             0.45f * g->s, GOLD_DEEP);
#undef LEAF_N
}

/* The cap, outlined on either dial as the leaf hands are, since it sits on
   their gold. */
static void centre_cap(canvas_t *c, const geo_t *g)
{
    vec_disc(c, g->cx + 0.6f * g->s, g->cy + 0.8f * g->s, 4.4f * g->s, SHADOW);
    vec_disc(c, g->cx, g->cy, 4.2f * g->s + HALO_PX, HALO);
    vec_disc(c, g->cx, g->cy, 4.2f * g->s, GOLD_LO);
    vec_disc(c, g->cx, g->cy, 3.4f * g->s, GOLD);
    vec_disc(c, g->cx - 0.8f * g->s, g->cy - 0.9f * g->s, 1.3f * g->s,
             mix565(GOLD, GOLD_HI, 0.8f));
    vec_disc(c, g->cx, g->cy, 0.9f * g->s, GOLD_DEEP);
}

/* The ground under a label on a photo dial, as one colour: the dial there
   with the grooves averaged out, as sub_ground, but the photo shows through
   the smoked sub-dial, so it is averaged over a grid across the label's
   box rather than taken at one point that might land on a star. (lx, ly)
   is the label's centre, units from the sub-dial's pivot (ox, oy). */
static uint16_t photo_label_ground(const geo_t *g, const uint16_t *photo,
                                   float ox, float oy, float lx, float ly,
                                   const vfont_style_t *f, const char *text)
{
    float hw = 0.5f * vfont_width(f, text) / g->s, hh = 0.5f * f->size / g->s;
    float acc[3] = { 0.0f, 0.0f, 0.0f };
    for (int j = -1; j <= 1; j++) {
        for (int i = -2; i <= 2; i++) {
            float col[3];
            dial_pixel(g, photo, ox + lx + 0.5f * hw * (float)i,
                       oy + ly + hh * (float)j, false, 2.0f / g->s, col);
            for (int q = 0; q < 3; q++) acc[q] += col[q] / 15.0f;
        }
    }
    int r5 = (int)(clamp01(acc[0] / 255.0f) * 31.0f + 0.5f);
    int g6 = (int)(clamp01(acc[1] / 255.0f) * 63.0f + 0.5f);
    int b5 = (int)(clamp01(acc[2] / 255.0f) * 31.0f + 0.5f);
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

/* A sub-dial label set again over a hand that has just been drawn across
   or beside it, where subdials() put it: a knockout a stroke wider than
   the lettering in the dial's own ground there, then the label. */
static void relabel(canvas_t *c, const geo_t *g, const uint16_t *photo,
                    float ox, float oy, float a, float inner, uint16_t colour,
                    const char *text)
{
    vfont_style_t f = scaled(&s_sub_font, g), k = f;
    k.weight += 1.4f * g->s;
    float r = label_radius(g, &f, a, inner, text);
    uint16_t ground = photo != NULL
        ? photo_label_ground(g, photo, ox, oy, r * sinf(a), -r * cosf(a), &f, text)
        : sub_ground(g, r * sinf(a), -r * cosf(a));
    sub_label(c, g, &k, ox, oy, a, r, ground, text);
    sub_label(c, g, &f, ox, oy, a, r, colour, text);
}

static void draw_moving(canvas_t *c, const face_state_t *s, const uint16_t *photo)
{
    geo_t g = geo_for(c->w, c->h);
    bool halo = photo != NULL;

    reserve(c, &g, s->battery_pct < 0 ? -1 : s->battery_pct, s->charging, halo);

    /* The day hand stops short of the lettering and today's day is set
       again over it in gold, so the day reads at a glance and the hand
       only confirms it. The date hand reaches its division, through the
       numerals; when it lies across or beside one, that numeral is set
       again over it (see sub_ground). */
    int wd = wrapi(s->weekday, 7);
    sub_hand(c, &g, -SUB_D, 0.0f, day_angle(wd), SUB_R - 17.5f, halo);
    relabel(c, &g, photo, -SUB_D, 0.0f, day_angle(wd), SUB_TICK_IN, GOLD_HI,
            s_days[wd]);

    int day = s->day < 1 ? 1 : (s->day > 31 ? 31 : s->day);
    sub_hand(c, &g, SUB_D, 0.0f, date_angle(day), SUB_R - 2.4f, halo);
    int near = (day + 2) / 5 * 5;               /* the nearest numeral */
    if (near >= 5 && near <= 30 && abs(near - day) <= 1) {
        char buf[8];
        snprintf(buf, sizeof buf, "%d", near);
        relabel(c, &g, photo, SUB_D, 0.0f, date_angle(near), SUB_MAJOR_IN, CREAM, buf);
    }

    int sec = wrapi(s->second, 60);
    sub_hand(c, &g, 0.0f, SUB_D, (float)sec * 6.0f * DEG, SUB_R - 1.8f, halo);

    int min = wrapi(s->minute, 60);
    int hr = wrapi(s->hour, 24) % 12;
    float ma = ((float)min + (float)sec / 60.0f) * 6.0f * DEG;
    float ha = ((float)hr + (float)min / 60.0f + (float)sec / 3600.0f) * 30.0f * DEG;
    /* Tails short enough to end under the cap: any longer and they stick
       out past it as pixel-wide whiskers at odd angles. */
    leaf_hand(c, &g, ha, HOUR_LEN, HOUR_W, 1.3f, 3.5f, photo != NULL);
    leaf_hand(c, &g, ma, MIN_LEN, MIN_W, 1.1f, 3.5f, photo != NULL);
    centre_cap(c, &g);
}

/* ---- the face ---------------------------------------------------------- */

void face_init(face_t *f, uint16_t *bg, int w, int h)
{
    if (f == NULL) return;
    f->bg = bg;
    f->w = w;
    f->h = h;
    f->built = false;
    f->moon_step = -1;
    f->rebuilds = 0;
    f->photo = NULL;
}

void face_set_photo(face_t *f, const uint16_t *rgb565)
{
    if (f == NULL || f->photo == rgb565) return;
    f->photo = rgb565;
    f->built = false;
}

static bool canvas_usable(const canvas_t *c)
{
    return c != NULL && c->fb != NULL && c->w > 0 && c->h > 0;
}

static void draw_full(canvas_t *c, const face_state_t *s, const uint16_t *photo)
{
    shade_static(c, moon_step_for(s->moon_phase), photo);
    draw_moving(c, s, photo);
}

void face_draw_full(canvas_t *c, const face_state_t *s)
{
    if (!canvas_usable(c) || s == NULL) return;
    draw_full(c, s, NULL);
}

void face_draw(face_t *f, canvas_t *c, const face_state_t *s)
{
    if (!canvas_usable(c) || s == NULL) return;
    if (f == NULL) {
        draw_full(c, s, NULL);
        return;
    }
    if (f->bg == NULL || f->w != c->w || f->h != c->h || f->bg == c->fb) {
        /* Drawn into the background itself, the hands are now in it: it
           no longer holds the bare dial, so the next draw must rebuild. */
        if (f->bg == c->fb) f->built = false;
        draw_full(c, s, f->photo);
        return;
    }
    int step = moon_step_for(s->moon_phase);
    if (!f->built || step != f->moon_step) {
        canvas_t bgc = *c;
        bgc.fb = f->bg;
        shade_static(&bgc, step, f->photo);
        f->built = true;
        f->moon_step = step;
        f->rebuilds++;
    }
    memcpy(c->fb, f->bg, (size_t)c->w * (size_t)c->h * sizeof(uint16_t));
    draw_moving(c, s, f->photo);
}

/* ---- the moon page ------------------------------------------------------ */

/* Days since 1970-01-01 for a civil date, and back; Howard Hinnant's
   algorithms, exact for any proleptic Gregorian date. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static const char *const s_months[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

/* The sky: near black, lifting to a faint blue round the moon, with a few
   stars placed by hash so they are the same every time. */
static void moon_sky(canvas_t *c, float mcx, float mcy, float mr, float glow)
{
    for (int j = 0; j < c->h; j++) {
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        for (int i = 0; i < c->w; i++) {
            float x = (float)i + 0.5f - mcx, y = (float)j + 0.5f - mcy;
            float d = sqrtf(x * x + y * y) - mr;
            float h = d > 0.0f ? expf(-d / (0.3f * mr)) * glow : glow;
            float v = 0.35f + 0.65f * (1.0f - (float)j / (float)c->h);
            float r = 3.0f + 5.0f * v + 26.0f * h;
            float gg = 5.0f + 9.0f * v + 36.0f * h;
            float b = 12.0f + 22.0f * v + 58.0f * h;
            row[i] = dither565(r, gg, b, i, j);
        }
    }
    /* Stars, kept off the moon and away from the lettering. */
    for (int k = 0; k < 70; k++) {
        float x = hash1(1000u + (uint32_t)k * 2u) * (float)c->w;
        float y = hash1(2001u + (uint32_t)k * 2u) * (float)c->h * 0.62f;
        float dx = x - mcx, dy = y - mcy;
        if (sqrtf(dx * dx + dy * dy) < mr + 10.0f) continue;
        float b = hash1(3003u + (uint32_t)k);
        uint16_t col = mix565(RGB(40, 50, 80), RGB(230, 226, 210), 0.25f + 0.6f * b * b);
        vec_disc(c, x, y, 0.45f + 0.5f * b * b, col);
    }
}

/* The moon texture, if any (face_set_moon_texture), and where its disc sits
   in it: assets/watch/moon.bin, NASA SVS's full moon, has the disc centred
   in the frame and 218 px across, so its centre is at (119.5, 139.5) in
   pixel-centre coordinates and its radius 109. It is sampled out to 107.5,
   a pixel and a half inside the limb, so the bilinear sample never takes
   in the black round it; the page's own anti-aliased edge makes the limb.
   The render is a little dim for a moon on a black page, so it is lifted
   by TEX_GAIN; and above TEX_KNEE each channel is rolled off smoothly
   towards 1 rather than clipped, so Tycho's rays and the southern
   highlands keep their detail at full instead of flattening into white. */
#define TEX_CX    119.5f
#define TEX_CY    139.5f
#define TEX_R     107.5f
#define TEX_GAIN    1.10f
#define TEX_KNEE    0.82f

static const uint16_t *s_moon_tex;

void face_set_moon_texture(const uint16_t *rgb565_240x280)
{
    s_moon_tex = rgb565_240x280;
}

/* The drawn moon's albedo at (x, y) on the unit disc, about 0.6 in the
   maria to 1 in the highlands: soft pools whose edges noise roughens, so
   they run into each other the way the real ones do, a fine grain, and two
   bright young craters, Tycho and Copernicus. */
static float drawn_albedo(float x, float y)
{
    static const float mare[][5] = {
        /* x, y, rx, ry on the unit disc, and depth */
        { -0.30f, -0.40f, 0.25f, 0.21f, 1.0f },   /* Imbrium */
        { 0.13f, -0.36f, 0.17f, 0.16f, 1.0f },    /* Serenitatis */
        { 0.28f, -0.10f, 0.21f, 0.17f, 1.0f },    /* Tranquillitatis */
        { 0.63f, -0.27f, 0.09f, 0.08f, 1.1f },    /* Crisium */
        { 0.52f, 0.14f, 0.10f, 0.15f, 0.8f },     /* Fecunditatis */
        { 0.34f, 0.25f, 0.07f, 0.07f, 0.7f },     /* Nectaris */
        { -0.56f, -0.05f, 0.22f, 0.36f, 0.85f },  /* Procellarum */
        { -0.14f, 0.33f, 0.14f, 0.11f, 0.8f },    /* Nubium */
        { -0.46f, 0.34f, 0.07f, 0.07f, 0.8f },    /* Humorum */
        { -0.10f, -0.70f, 0.34f, 0.09f, 0.45f },  /* Frigoris */
        { 0.03f, 0.02f, 0.07f, 0.08f, 0.5f },     /* Sinus Medii */
        { -0.20f, -0.16f, 0.10f, 0.09f, 0.5f },   /* between Imbrium and Nubium */
    };
    float m = 0.0f;
    for (int k = 0; k < (int)(sizeof mare / sizeof mare[0]); k++) {
        float ex = (x - mare[k][0]) / mare[k][2];
        float ey = (y - mare[k][1]) / mare[k][3];
        m += mare[k][4] * expf(-1.3f * (ex * ex + ey * ey));
    }
    float n = fbm(x * 6.0f + 11.0f, y * 6.0f + 7.0f, 41u);
    float mask = smooth(0.15f, 0.85f, m + 0.55f * (n - 0.5f));
    float grain = fbm(x * 22.0f, y * 22.0f, 77u) - 0.5f;
    float albedo = (1.0f - 0.38f * mask) * (1.0f + 0.08f * grain);
    float tx = x + 0.12f, ty = y - 0.70f, cxp = x + 0.30f, cyp = y + 0.14f;
    albedo += 0.10f * expf(-(tx * tx + ty * ty) / 0.0016f)
            + 0.07f * expf(-(cxp * cxp + cyp * cyp) / 0.0014f);
    return albedo;
}

/*
 * The moon itself, as a lit sphere, always the whole disc: the sun's
 * direction worked from the phase (behind the moon at new, behind us at
 * full, from the right while waxing, as the northern hemisphere sees it),
 * each pixel lit by the sphere's normal there with a soft terminator, and
 * the rest of the disc in earthshine. Only the light moves; the disc stays
 * round at every phase, which is what keeps a gibbous moon from reading as
 * an oval with its dark limb lost in the sky.
 *
 * The surface is the full-moon photograph when there is one, scaled onto
 * the disc (a 2x2 supersample of bilinear taps, since it shrinks by about
 * a half), and the drawn moon otherwise.
 */
static void big_moon(canvas_t *c, float cx, float cy, float R, double phase,
                     const uint16_t *tex)
{
    double p = phase - floor(phase);
    if (!(p >= 0.0 && p < 1.0)) p = 0.0;
    float sxv = sinf(TWO_PI * (float)p), szv = -cosf(TWO_PI * (float)p);

    int x0 = (int)floorf(cx - R - 1.0f), x1 = (int)ceilf(cx + R + 1.0f);
    int y0 = (int)floorf(cy - R - 1.0f), y1 = (int)ceilf(cy + R + 1.0f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > c->w) x1 = c->w;
    if (y1 > c->h) y1 = c->h;

    /* Earthshine is a cool grey-blue, of about unit luminance here. */
    static const float cool[3] = { 0.80f * 255.0f, 0.94f * 255.0f, 1.16f * 255.0f };
    float q = 0.25f / R;                /* a quarter pixel on the unit disc */

    for (int j = y0; j < y1; j++) {
        uint16_t *row = c->fb + (size_t)j * (size_t)c->w;
        for (int i = x0; i < x1; i++) {
            float x = ((float)i + 0.5f - cx) / R, y = ((float)j + 0.5f - cy) / R;
            float d2 = x * x + y * y;
            float d = sqrtf(d2);
            float cov = clamp01((1.0f - d) * R + 0.5f);
            if (cov <= 0.0f) continue;
            float nz = d2 < 1.0f ? sqrtf(1.0f - d2) : 0.0f;

            /* The full moon's own colour here, 0..1 or a little over. */
            float alb[3] = { 0.0f, 0.0f, 0.0f };
            if (tex != NULL) {
                float in = d > 1.0f ? 1.0f / d : 1.0f;  /* the rim pixels */
                for (int k = 0; k < 4; k++) {
                    float sx = (x + ((k & 1) ? q : -q)) * in;
                    float sy = (y + ((k >> 1) ? q : -q)) * in;
                    float t[3];
                    photo_sample(tex, TEX_CX + sx * TEX_R, TEX_CY + sy * TEX_R, false, t);
                    for (int m = 0; m < 3; m++)
                        alb[m] += t[m] * (TEX_GAIN / 255.0f / 4.0f);
                }
                for (int m = 0; m < 3; m++)
                    if (alb[m] > TEX_KNEE)
                        alb[m] = TEX_KNEE + (1.0f - TEX_KNEE)
                               * tanhf((alb[m] - TEX_KNEE) / (1.0f - TEX_KNEE));
            } else {
                float a = drawn_albedo(x, y) * (0.88f + 0.12f * nz);
                alb[0] = a * (234.0f / 255.0f);
                alb[1] = a * (230.0f / 255.0f);
                alb[2] = a * (216.0f / 255.0f);
            }
            float lum = 0.2126f * alb[0] + 0.7152f * alb[1] + 0.0722f * alb[2];

            /* The lunar soil scatters light by roughly the Lommel-Seeliger
               law, cos i / (cos i + cos e), not as a matte ball: a full
               moon is nearly as bright at its limb as at its centre (which
               the photo already shows), while a half or crescent moon dims
               gradually into its terminator. That gradient lets the
               terminator itself stay fairly crisp. `ls` is the law's ratio
               to its value at full, 1 there. */
            float lam = x * sxv + nz * szv;             /* cos i */
            float lit = smooth(-0.07f, 0.09f, lam);
            float ls = lam > 0.0f ? clamp01(2.0f * lam / (lam + nz)) : 0.0f;
            /* The photo's surface takes more of the law than the drawn
               one: its detail then fades into the terminator as the real
               moon's does, where the drawn moon's smooth maria would only
               look smudged. */
            float bright = tex != NULL ? lit * (0.40f + 0.60f * ls)
                                       : lit * (0.62f + 0.38f * ls);

            /* Earthshine: the unlit part at 12 to 17 per cent, the maria
               still faintly there. The Earth lights it from almost behind
               us, so it is nearly even across the disc; a slight lift in
               the last few pixels gives the dark limb a clean edge against
               the sky. Any broader and the dark side reads as a glass
               bubble, bright at its rim and hollow in the middle. */
            float es = (0.080f + 0.10f * lum) + 0.05f * smooth(0.88f, 1.0f, d);
            float dark = (1.0f - lit) * es;

            float r = 255.0f * bright * alb[0] + dark * cool[0];
            float gg = 255.0f * bright * alb[1] + dark * cool[1];
            float b = 255.0f * bright * alb[2] + dark * cool[2];

            uint16_t px565 = dither565(r, gg, b, i, j);
            row[i] = cov >= 1.0f ? px565 : mix565(row[i], px565, cov);
        }
    }
}

/* "SAT 26 SEP" for a Unix time shown at a UTC offset; and how many local
   days ahead of `today` it falls. False if the time is not set. */
/* Past this (about 34,800 years on) a time is taken as unset, which also
   keeps unix_utc + offset below int64's overflow. */
#define LOCAL_DATE_MAX ((int64_t)1 << 40)

static bool local_date(int64_t unix_utc, int32_t offset, char *out, size_t n,
                       int64_t today, int64_t *ahead)
{
    if (unix_utc <= 0 || unix_utc > LOCAL_DATE_MAX) return false;
    int64_t days = floor_div(unix_utc + (int64_t)offset, 86400);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    int wd = (int)(((days % 7) + 11) % 7);         /* 1970-01-01, a Thursday */
    snprintf(out, n, "%s %d %s", s_days[wd], d, s_months[m - 1]);
    *ahead = days - today;
    return true;
}

/*
 * The moon page's layout, in design units from the panel's centre. The
 * moon is centred a third of the way down the panel; below it the name,
 * a gilt rule, the age and lit fraction, and the two columns of dates,
 * spaced so the block sits evenly between the moon and the foot.
 */
#define MP_MOON_Y   (-140.0f / 3.0f)    /* y = h/3 on the 240x280 panel */
#define MP_MOON_R    72.0f
#define MP_NAME_Y    44.0f
#define MP_RULE_Y    56.0f
#define MP_INFO_Y    68.0f
#define MP_COL_Y     85.0f              /* the columns' labels */
#define MP_DATE_DY   14.0f              /* the dates, below the labels */
#define MP_REL_DY    26.0f              /* "IN 2 DAYS", below the labels */

static void moon_column(canvas_t *c, const geo_t *g, float x, const char *label,
                        int64_t when, int32_t offset, int64_t today)
{
    vfont_style_t lab = { 5.2f * g->s, 0.8f * g->s, 1.2f * g->s };
    vfont_style_t big = { 8.0f * g->s, 1.05f * g->s, 0.9f * g->s };
    vfont_style_t sub = { 5.0f * g->s, 0.8f * g->s, 0.8f * g->s };
    char date[32], rel[32];
    int64_t ahead = 0;

    vfont_draw(c, &lab, x, py(g, MP_COL_Y), 0.0f, VFONT_CENTRE, GOLD, label);
    if (!local_date(when, offset, date, sizeof date, today, &ahead)) {
        vfont_draw(c, &big, x, py(g, MP_COL_Y + MP_DATE_DY), 0.0f, VFONT_CENTRE,
                   CREAM, "-");
        return;
    }
    vfont_draw(c, &big, x, py(g, MP_COL_Y + MP_DATE_DY), 0.0f, VFONT_CENTRE,
               CREAM, date);
    if (ahead <= 0) snprintf(rel, sizeof rel, "TODAY");
    else if (ahead == 1) snprintf(rel, sizeof rel, "TOMORROW");
    else snprintf(rel, sizeof rel, "IN %d DAYS", (int)(ahead > 99 ? 99 : ahead));
    vfont_draw(c, &sub, x, py(g, MP_COL_Y + MP_REL_DY), 0.0f, VFONT_CENTRE,
               CREAM_SOFT, rel);
}

void face_draw_moon_page(canvas_t *c, const face_state_t *s)
{
    if (!canvas_usable(c) || s == NULL) return;
    geo_t g = geo_for(c->w, c->h);

    double p = s->moon_phase - floor(s->moon_phase);
    if (!(p >= 0.0 && p < 1.0)) p = 0.0;
    float illum = 0.5f * (1.0f - cosf(TWO_PI * (float)p));

    float mcx = px(&g, 0.0f), mcy = py(&g, MP_MOON_Y), mr = MP_MOON_R * g.s;
    /* The sky's glow round the moon stays faint even at full, so it never
       comes up to the earthshine at the limb. */
    moon_sky(c, mcx, mcy, mr, 0.02f + 0.11f * illum);
    big_moon(c, mcx, mcy, mr, p, s_moon_tex);

    vfont_style_t name = { 10.0f * g.s, 1.25f * g.s, 2.2f * g.s };
    vfont_draw(c, &name, px(&g, 0.0f), py(&g, MP_NAME_Y), 0.0f, VFONT_CENTRE, CREAM,
               moonphase_name(p));

    /* A short gilt rule with a lozenge. */
    float ry = py(&g, MP_RULE_Y);
    vec_line(c, px(&g, -30.0f), ry, px(&g, -5.0f), ry, 0.6f * g.s, GOLD_LO);
    vec_line(c, px(&g, 5.0f), ry, px(&g, 30.0f), ry, 0.6f * g.s, GOLD_LO);
    float dia[8] = { px(&g, 0.0f), ry - 2.2f * g.s, px(&g, 2.2f), ry,
                     px(&g, 0.0f), ry + 2.2f * g.s, px(&g, -2.2f), ry };
    vec_polygon(c, dia, 4, GOLD);

    /* Age and lit fraction, either side of a dot. */
    char left[32], right[32];
    double age = s->moon_age_days;
    if (!(age >= 0.0 && age < 40.0)) age = p * MOONPHASE_SYNODIC_DAYS;
    /* In whole tenths, so the printed width is bounded for the compiler's
       truncation check as well as in fact. */
    int tenths = (int)(age * 10.0 + 0.5);
    snprintf(left, sizeof left, "%d.%d DAYS OLD", tenths / 10, tenths % 10);
    snprintf(right, sizeof right, "%d%% LIT", (int)(illum * 100.0f + 0.5f));
    vfont_style_t info = { 5.6f * g.s, 0.85f * g.s, 1.1f * g.s };
    float wl = vfont_width(&info, left), wr = vfont_width(&info, right);
    float gap = 12.0f * g.s;
    float x0 = px(&g, 0.0f) - 0.5f * (wl + gap + wr);
    float iy = py(&g, MP_INFO_Y);
    vfont_draw(c, &info, x0, iy, 0.0f, VFONT_LEFT, CREAM_SOFT, left);
    vec_disc(c, x0 + wl + 0.5f * gap, iy, 0.9f * g.s, GOLD_LO);
    vfont_draw(c, &info, x0 + wl + gap, iy, 0.0f, VFONT_LEFT, CREAM_SOFT, right);

    /* The next full and new moons, in two columns. */
    /* The year clamped too: days_from_civil's y - 1 and y - 399 would
       overflow an int near INT_MIN. */
    int year = s->year < -1000000 ? -1000000 : (s->year > 1000000 ? 1000000 : s->year);
    int64_t today = days_from_civil(year, s->month < 1 ? 1 : (s->month > 12 ? 12 : s->month),
                                    s->day < 1 ? 1 : (s->day > 31 ? 31 : s->day));
    vec_line(c, px(&g, 0.0f), py(&g, MP_COL_Y - 3.0f), px(&g, 0.0f),
             py(&g, MP_COL_Y + MP_REL_DY + 3.0f), 0.6f * g.s, GOLD_LO);
    /* NEXT rather than MOON: at new moon the title already says NEW MOON. */
    moon_column(c, &g, px(&g, -56.0f), "NEXT FULL", s->next_full, s->utc_offset, today);
    moon_column(c, &g, px(&g, 56.0f), "NEXT NEW", s->next_new, s->utc_offset, today);
}
