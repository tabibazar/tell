#ifndef FACE_H
#define FACE_H

#include "canvas.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * watch's analog face: a grand complication on a navy sunburst dial. Leaf
 * hands for the hours and minutes; small seconds at 6, jumping once a
 * second like a quartz movement; the day of the week at 9 and the date at 3,
 * each on its own sub-dial with its own hand; a moon-phase aperture at 12,
 * with a power-reserve scale arched over it for the battery.
 *
 * The dial is either that sunburst or a photograph (face_set_photo): the
 * photo becomes the bottom layer of the dial, dimmed and its highlights
 * rolled off so the gold and cream stay the brightest things on it,
 * darkened under the minute track and ending at it, with each sub-dial a
 * smoked disc over it; the hands and indices get a fine dark outline so
 * they read over bright cloud anywhere.
 *
 * Also the moon page: the moon large, lit as it is tonight, with its name,
 * its age and the dates of the next full and new moons. The moon is always
 * the whole disc, its dark part in earthshine, so only the light moves
 * across it; its surface is drawn, or taken from a photograph of the full
 * moon (face_set_moon_texture).
 *
 * Laid out for the 240x280 portrait panel. Any other size gets the same
 * face scaled to fit and centred, so nothing ever lands off the canvas.
 *
 * The dial is expensive and the hands are cheap. Its texture is shaded pixel
 * by pixel (the sunburst, the snailed sub-dials, the starry moon disc), which
 * is far too slow to repeat every second, so it is drawn once into a
 * background buffer the caller provides and copied out each second, with
 * only the moving parts drawn over it. The background is only redrawn when
 * the moon disc turns a step, every few hours. The day and date hands and
 * the power reserve's level are drawn with the hands, so a new day or a new
 * battery reading costs nothing extra.
 *
 * Pure: no clock, no hardware, no allocation. The caller brings the time.
 * Not reentrant (vector.c's polygon rows are static): draw from one task.
 */

typedef struct {
    /* Local time, 24-hour. Out-of-range values are wrapped, not trusted. */
    int hour, minute, second;
    /* Local date. */
    int year, month, day;
    /* 0 = Sunday .. 6 = Saturday. */
    int weekday;

    /* From moonphase_compute: 0 new, 0.5 full; and days since new. */
    double moon_phase;
    double moon_age_days;

    /* 0..100, or -1 when there is no reading. */
    int battery_pct;
    bool charging;

    /* The moon page only: the next new and full moons as Unix UTC seconds
       (moonphase_t's next_new and next_full), and the local offset from
       UTC in seconds, east positive, so their dates print as local dates. */
    int64_t next_new, next_full;
    int32_t utc_offset;
} face_state_t;

/* The photographs this takes (dial photos and the moon texture): exactly
   FACE_PHOTO_W x FACE_PHOTO_H RGB565 pixels, row-major, top row first, one
   native-endian uint16_t each -- the canvas framebuffer's own layout. */
#define FACE_PHOTO_W 240
#define FACE_PHOTO_H 280

typedef struct {
    uint16_t *bg;        /* w*h pixels, the caller's */
    int w, h;
    bool built;          /* bg holds the dial for moon_step and photo */
    int moon_step;       /* the moon disc's angle when bg was drawn */
    uint32_t rebuilds;   /* how many times bg has been drawn; for tests */
    const uint16_t *photo;  /* the dial photo, or NULL for the sunburst */
} face_t;

/* Takes a background buffer of w*h pixels, which must stay valid while `f`
   is used. Nothing is drawn until the first face_draw. `bg` may be NULL, and
   then every face_draw shades the whole dial afresh. Starts with no photo:
   the sunburst dial. */
void face_init(face_t *f, uint16_t *bg, int w, int h);

/* A photograph for the dial, FACE_PHOTO_W x FACE_PHOTO_H (see above), laid
   under everything else and mapped to the dial as the face itself is, so on
   the 240x280 panel it lands pixel for pixel. NULL goes back to the
   sunburst. The pixels are read whenever the background is rebuilt, so
   they must stay valid (and unchanged) while `f` uses them: a flash-mapped
   const array is ideal. Setting a different photo invalidates the cached
   background, so the next face_draw rebuilds it once; setting the same one
   again costs nothing. */
void face_set_photo(face_t *f, const uint16_t *rgb565);

/* The face at this moment into `c`: the dial, redrawn into the background
   only if the moon has moved on, then copied out, then the hands. A canvas
   of a different size from the background is drawn from scratch, as is one
   whose framebuffer is the background itself (which then no longer holds
   the bare dial, so the next draw rebuilds it). */
void face_draw(face_t *f, canvas_t *c, const face_state_t *s);

/* The same face with no background: everything shaded straight into `c`.
   Slow; for the host renders, and the check that the cache changes
   nothing. Always the sunburst dial: this takes no face_t and so no photo.
   (A face_t with a NULL background draws its photo afresh each time.) */
void face_draw_full(canvas_t *c, const face_state_t *s);

/* The moon page: the moon, its name, age and lit fraction, and the dates of
   the next full and new moon from s->next_full and s->next_new. Uses the
   moon and date fields of `s`, not the time. The moon is centred across the
   panel and a third of the way down it (y = 93 on 240x280). */
void face_draw_moon_page(canvas_t *c, const face_state_t *s);

/* The moon page's surface: a photograph of the full moon, FACE_PHOTO_W x
   FACE_PHOTO_H (see above), with the disc centred in it, 218 px across, as
   assets/watch/moon.bin has it. The disc is scaled onto the page's moon and
   then lit for the phase. NULL (the default) goes back to the drawn moon.
   One for the whole module, not per face; the pixels must stay valid while
   it is set. */
void face_set_moon_texture(const uint16_t *rgb565_240x280);

#endif /* FACE_H */
