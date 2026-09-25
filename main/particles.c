#include "particles.h"

#include "palette.h"

/* Drag per second, the bulk viscosity of the body. It can be this lively only
   because the grains genuinely separate: while crowding was approximated by a
   density grid, anything above about 0.1 fed the corrections back as velocity
   and the body hummed instead of settling. */
#define DRAG 0.60f
/* How strongly a grain is pulled toward the mean velocity of its cell, per
   frame. This is what makes it a liquid rather than a cloud: neighbours
   travel together, so the body slumps and levels instead of every grain
   going its own way. It is applied once per step, so a frame taken in k
   steps uses the per-step share that compounds to the same 0.25 --
   1 - 0.75^(1/k) -- or the liquid would thicken as the board tilts and the
   step count rises. One entry per step count, up to SUBSTEPS_MAX. */
#define VISCOSITY 0.25f
static const float viscosity_per_step[] = { VISCOSITY, 0.1340f, 0.0914f };

/* How far gravity may sink a grain into the pile in one step, in pixels:
   |g| * dt^2, since velocity is re-derived from where the grain ended up.
   Support climbs from the floor about one layer per separation pass, so
   while it climbs, every grain above keeps sinking; past about 0.6 px a
   step the deepest pile here never catches up and shimmers for ever. That
   pile is watch tipped corner-down -- 395 grains wedged into a 240x280
   corner, about twenty layers along gravity -- which settles at 0.63 px a
   step and shimmers at 0.79. lilly and wave on their sides, or tipped, are
   the same failure at their own 1134 and 1147 in one step a frame. 0.4
   leaves a third in hand. */
#define SINK_MAX 0.4f
/* The most steps one call may take. A long stall is clamped by the caller,
   not caught up here: 0.2 s at 1200 would want twelve, and solving twelve
   times makes the next frame late too. Three settles everything the panels
   here ask for down to 20 fps. */
#define SUBSTEPS_MAX 3
_Static_assert(sizeof viscosity_per_step / sizeof viscosity_per_step[0] == SUBSTEPS_MAX,
               "one viscosity per step count");
/* How many times the overlaps are resolved per step. One pass cannot settle
   a stack: separating the grain at the bottom crowds the one above it, which
   would otherwise not be dealt with until the next step. */
#define RELAX_PASSES 3
/* Each pass removes this share of an overlap. Below one, so a grain wedged
   between two others does not ping between them. */
#define STIFFNESS 0.9f
/* A per-step nudge, in pixels per second. Barely anything: enough that the
   surface never becomes a frozen straight line, not enough to stop the body
   settling. */
#define JITTER 0.3f

#define GRAIN_SIZE 4

static uint32_t next_rand(particles_t *s)
{
    s->rng = s->rng * 1103515245u + 12345u;
    return s->rng >> 16;
}

static float frand(particles_t *s, float lo, float hi)
{
    return lo + (float)(next_rand(s) % 10000u) / 10000.0f * (hi - lo);
}

int particles_for(int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    int n = w * h / PARTICLES_PIXELS_EACH;
    return n > PARTICLES_MAX ? PARTICLES_MAX : n;
}

void particles_init(particles_t *s, int n, int w, int h, uint32_t seed)
{
    if (n > PARTICLES_MAX) n = PARTICLES_MAX;
    if (n < 0) n = 0;
    s->n = n;
    s->w = w;
    s->h = h;
    s->rng = seed ? seed : 1u;

    s->gw = w / PARTICLES_CELL + 1;
    s->gh = h / PARTICLES_CELL + 1;
    if (s->gw > PARTICLES_GRID_W) s->gw = PARTICLES_GRID_W;
    if (s->gh > PARTICLES_GRID_H) s->gh = PARTICLES_GRID_H;

    /* Six colours from the project's Okabe-Ito set, so the animation belongs
       to the same palette as the charts. */
    static const uint16_t colours[6] = {
        PAL_A0, PAL_A1, PAL_A2, PAL_A3, PAL_A4, PAL_A5
    };

    for (int i = 0; i < n; i++) {
        s->p[i].x = frand(s, 1.0f, (float)w - 1.0f);
        s->p[i].y = frand(s, 1.0f, (float)h - 1.0f);
        s->p[i].vx = frand(s, -20.0f, 20.0f);
        s->p[i].vy = frand(s, -20.0f, 20.0f);
        s->p[i].ox = s->p[i].x;
        s->p[i].oy = s->p[i].y;
        s->p[i].colour = colours[next_rand(s) % 6u];
    }
}

static int cell_index(const particles_t *s, float x, float y)
{
    int cx = (int)x / PARTICLES_CELL;
    int cy = (int)y / PARTICLES_CELL;
    if (cx < 0) cx = 0; else if (cx >= s->gw) cx = s->gw - 1;
    if (cy < 0) cy = 0; else if (cy >= s->gh) cy = s->gh - 1;
    return cy * s->gw + cx;
}

/* Groups the grain indices by cell, so the separation pass can walk a cell's
   grains contiguously instead of searching. A counting sort: count, prefix
   sum, place. */
static void bucket(particles_t *s)
{
    int cells = s->gw * s->gh;
    for (int i = 0; i <= cells; i++) s->head[i] = 0;
    for (int i = 0; i < cells; i++) { s->vgx[i] = 0.0f; s->vgy[i] = 0.0f; }

    for (int i = 0; i < s->n; i++) s->head[cell_index(s, s->p[i].x, s->p[i].y) + 1]++;
    for (int i = 0; i < cells; i++) s->head[i + 1] = (uint16_t)(s->head[i + 1] + s->head[i]);
    for (int i = 0; i < cells; i++) s->fill[i] = s->head[i];

    for (int i = 0; i < s->n; i++) {
        int k = cell_index(s, s->p[i].x, s->p[i].y);
        s->order[s->fill[k]++] = (uint16_t)i;
        s->vgx[k] += s->p[i].vx;
        s->vgy[k] += s->p[i].vy;
    }
    for (int i = 0; i < cells; i++) {
        int held = s->head[i + 1] - s->head[i];
        if (held > 0) { s->vgx[i] /= (float)held; s->vgy[i] /= (float)held; }
    }
}

/* Pushes any two grains that overlap apart, each by half of the overlap.
   Every pair is visited once: a cell against itself, and against the four
   neighbours on one side only, which covers all nine without doing any of
   them twice. */
static void separate(particles_t *s)
{
    const float r = 2.0f * PARTICLES_RADIUS;
    const float r2 = r * r;
    const float inv_r2 = 1.0f / r2;
    (void)r;

    for (int cy = 0; cy < s->gh; cy++) {
        for (int cx = 0; cx < s->gw; cx++) {
            int a = cy * s->gw + cx;
            for (int ai = s->head[a]; ai < s->head[a + 1]; ai++) {
                particle_t *p = &s->p[s->order[ai]];

                /* dx, dy over the half-neighbourhood plus self. */
                static const int nx[5] = { 0, 1, -1, 0, 1 };
                static const int ny[5] = { 0, 0,  1, 1, 1 };
                for (int d = 0; d < 5; d++) {
                    int bx = cx + nx[d], by = cy + ny[d];
                    if (bx < 0 || by < 0 || bx >= s->gw || by >= s->gh) continue;
                    int b = by * s->gw + bx;
                    int from = (d == 0) ? ai + 1 : s->head[b];
                    for (int bi = from; bi < s->head[b + 1]; bi++) {
                        particle_t *q = &s->p[s->order[bi]];
                        float dx = q->x - p->x, dy = q->y - p->y;
                        float d2 = dx * dx + dy * dy;
                        if (d2 >= r2) continue;
                        if (d2 < 0.01f) {
                            /* Exactly on top of each other: nudge them apart
                               along a fixed axis rather than dividing by
                               zero. Which axis does not matter; the next
                               frame will sort it out. */
                            dx = 0.1f; dy = 0.0f; d2 = 0.01f;
                        }
                        /* A square-law push, so no square root. The Xtensa
                           has no hardware sqrt and does it in software; with
                           one per overlapping pair per pass it dominated the
                           frame, at 58 ms against a 33 ms budget. This is a
                           softer spring but it costs a multiply. */
                        float push = (r2 - d2) * inv_r2 * STIFFNESS * 0.5f;
                        p->x -= dx * push;
                        p->y -= dy * push;
                        q->x += dx * push;
                        q->y += dy * push;
                    }
                }
            }
        }
    }
}

static void step_once(particles_t *s, float gx, float gy, float dt, float viscosity)
{
    /* Drag as a per-second multiplier applied linearly over dt. At the frame
       rates this runs at, dt is small enough that the approximation is
       indistinguishable from the exponential and much cheaper. */
    float damp = 1.0f - (1.0f - DRAG) * dt;
    if (damp < 0.0f) damp = 0.0f;

    /* The walls are where a grain's block still fits, not the panel's edge.
       Each grain is drawn as a block from its position down and to the
       right, so the floor used to be h itself, and a grain resting there was
       drawn entirely below the last row -- the whole bottom layer of a
       settled pile, about a fifth of the grains, was clipped away, and on
       lilly and wave the sand stood on a dark strip five rows deep with
       4 of 320 pixels lit. Stopping a block short of the far walls keeps
       every grain whole and on the panel, and the pile on the floor. */
    float maxx = s->w > GRAIN_SIZE ? (float)(s->w - GRAIN_SIZE) : 0.0f;
    float maxy = s->h > GRAIN_SIZE ? (float)(s->h - GRAIN_SIZE) : 0.0f;
    float inv_dt = dt > 0.0f ? 1.0f / dt : 0.0f;

    /* Predict where each grain would go on its own. */
    bucket(s);
    for (int i = 0; i < s->n; i++) {
        particle_t *p = &s->p[i];
        int k = cell_index(s, p->x, p->y);

        p->ox = p->x;
        p->oy = p->y;

        p->vx += gx * dt;
        p->vy += gy * dt;
        /* Travel with the neighbours, not through them. */
        p->vx += (s->vgx[k] - p->vx) * viscosity;
        p->vy += (s->vgy[k] - p->vy) * viscosity;
        p->vx = p->vx * damp + frand(s, -JITTER, JITTER);
        p->vy = p->vy * damp + frand(s, -JITTER, JITTER);

        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }

    /* Then make the positions legal: no overlaps, nothing outside the panel.
       Moving grains rather than pushing them is what lets the body come to
       rest -- a force overshoots and hums for ever. */
    for (int pass = 0; pass < RELAX_PASSES; pass++) {
        if (pass > 0) bucket(s);
        separate(s);
        for (int i = 0; i < s->n; i++) {
            particle_t *p = &s->p[i];
            if (p->x < 0.0f) p->x = 0.0f; else if (p->x > maxx) p->x = maxx;
            if (p->y < 0.0f) p->y = 0.0f; else if (p->y > maxy) p->y = maxy;
        }
    }

    /* Velocity is whatever actually happened, derived once at the end. A
       grain held up by the body below it ends the frame where it began and
       therefore at rest, which is what settling means. Crediting each
       correction to the velocity as it is applied instead would double-count,
       because a later pass corrects what an earlier one did. */
    for (int i = 0; i < s->n; i++) {
        particle_t *p = &s->p[i];
        p->vx = (p->x - p->ox) * inv_dt;
        p->vy = (p->y - p->oy) * inv_dt;
    }
}

int particles_substeps(float gx, float gy, float dt)
{
    /* The smallest k with |g| (dt/k)^2 <= SINK_MAX, compared squared so
       there is no square root: the file has none, and needs no libm. */
    float g2 = gx * gx + gy * gy;
    int k = 1;
    while (k < SUBSTEPS_MAX) {
        float h = dt / (float)k;
        if (g2 * (h * h) * (h * h) <= SINK_MAX * SINK_MAX) break;
        k++;
    }
    return k;
}

void particles_step(particles_t *s, float gx, float gy, float dt)
{
    /* The caller hands over a whole frame, whatever its length; the pile
       only comes to rest if no single step sinks it too far, so the frame
       is cut up here rather than trusted to arrive short enough. */
    int k = particles_substeps(gx, gy, dt);
    for (int i = 0; i < k; i++)
        step_once(s, gx, gy, dt / (float)k, viscosity_per_step[k - 1]);
}

void particles_swirl(particles_t *s, float rate)
{
    float cx = (float)s->w / 2.0f;
    float cy = (float)s->h / 2.0f;
    for (int i = 0; i < s->n; i++) {
        float rx = s->p[i].x - cx;
        float ry = s->p[i].y - cy;
        s->p[i].vx += -ry * rate;
        s->p[i].vy +=  rx * rate;
    }
}

void particles_agitate(particles_t *s, float speed)
{
    if (speed <= 0.0f) return;
    for (int i = 0; i < s->n; i++) {
        s->p[i].vx += frand(s, -speed, speed);
        s->p[i].vy += frand(s, -speed, speed);
    }
}

void particles_draw(const particles_t *s, canvas_t *c)
{
    canvas_clear(c);
    for (int i = 0; i < s->n; i++)
        canvas_fill_rect(c, (int)s->p[i].x, (int)s->p[i].y,
                         GRAIN_SIZE, GRAIN_SIZE, s->p[i].colour);
}
