#include "fluid.h"

#include <math.h>
#include <string.h>

#include "palette.h"

/* Tuned on the host renders (test_fluid), at watch's 240x280. */
#ifndef REST_DENSITY
#define REST_DENSITY  1.6f
#endif
#ifndef STIFF
#define STIFF         1200.0f     /* pressure, px^2/s^2 per unit density */
#endif
#ifndef STIFF_NEAR
#define STIFF_NEAR    2500.0f
#endif
#ifndef VISC_LIN
#define VISC_LIN      1.2f
#endif
#ifndef VISC_QUAD
#define VISC_QUAD     0.02f
#endif       /* beta: damps drops flying straight at each other */
#ifndef SUBSTEP
#define SUBSTEP       (1.0f / 60.0f)   /* watch has the time for two or three a frame */
#endif
#ifndef FLUID_MAX_STEPS
#define FLUID_MAX_STEPS 3
#endif
#define WALL          3.0f        /* how close a drop's centre comes to a wall */

static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

int fluid_for(int w, int h)
{
    /* 300 on watch's 240x280: what she moves in real time at 240 MHz. */
    int n = (int)((float)(w * h) / 224.0f);
    return n > FLUID_MAX ? FLUID_MAX : n;
}

void fluid_init(fluid_t *f, int n, int w, int h, uint32_t seed)
{
    memset(f, 0, sizeof *f);
    if (n > FLUID_MAX) n = FLUID_MAX;
    f->n = n;
    f->w = w > 240 ? 240 : w;
    f->h = h > 280 ? 280 : h;
    uint32_t s = seed ? seed : 0x9E3779B9u;
    float sp = 7.2f;
    int cols = (int)((f->w - 2 * WALL) / sp);
    for (int i = 0; i < n; i++) {
        f->colour[i] = (uint8_t)(rnd(&s) % 6u);
        int cx = i % cols, cy = i / cols;
        f->x[i] = WALL + sp * (cx + 0.5f) + (rnd(&s) % 100) * 0.01f;
        f->y[i] = f->h - WALL - sp * (cy + 0.5f);
        if (f->y[i] < WALL) f->y[i] = WALL;
    }
}

static void bucket(fluid_t *f)
{
    for (int c = 0; c < FLUID_GRID_W * FLUID_GRID_H; c++) f->head[c] = -1;
    for (int i = 0; i < f->n; i++) {
        int gx = (int)(f->x[i] / FLUID_RADIUS), gy = (int)(f->y[i] / FLUID_RADIUS);
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx >= FLUID_GRID_W) gx = FLUID_GRID_W - 1;
        if (gy >= FLUID_GRID_H) gy = FLUID_GRID_H - 1;
        int c = gy * FLUID_GRID_W + gx;
        f->next[i] = f->head[c];
        f->head[c] = (int16_t)i;
    }
    /* Each drop's neighbours within FLUID_RADIUS, found once a step from the
       predicted positions; relaxation moves them less than a radius. */
    for (int i = 0; i < f->n; i++) {
        int gx = (int)(f->x[i] / FLUID_RADIUS), gy = (int)(f->y[i] / FLUID_RADIUS), k = 0;
        for (int yy = gy - 1; yy <= gy + 1; yy++) {
            if (yy < 0 || yy >= FLUID_GRID_H) continue;
            for (int xx = gx - 1; xx <= gx + 1; xx++) {
                if (xx < 0 || xx >= FLUID_GRID_W) continue;
                for (int j = f->head[yy * FLUID_GRID_W + xx]; j >= 0; j = f->next[j]) {
                    if (j == i || k >= FLUID_NEIGH) continue;
                    float dx = f->x[j] - f->x[i], dy = f->y[j] - f->y[i];
                    if (dx * dx + dy * dy < FLUID_RADIUS * FLUID_RADIUS) f->nb[i][k++] = (int16_t)j;
                }
            }
        }
        f->nn[i] = (uint8_t)k;
    }
}

static void substep(fluid_t *f, float gx, float gy, float dt)
{
    const int n = f->n;
    const float inv_h = 1.0f / FLUID_RADIUS;
    for (int i = 0; i < n; i++) {
        f->vx[i] += gx * dt;
        f->vy[i] += gy * dt;
    }
    bucket(f);
    /* Viscosity: drops closing on each other give up some of that speed. */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < f->nn[i]; k++) {
            int j = f->nb[i][k];
            if (j < i) continue;
            float dx = f->x[j] - f->x[i], dy = f->y[j] - f->y[i];
            float r2 = dx * dx + dy * dy;
            if (r2 < 1e-8f) continue;
            float ir = 1.0f / sqrtf(r2);
            float ux = dx * ir, uy = dy * ir;
            float u = (f->vx[i] - f->vx[j]) * ux + (f->vy[i] - f->vy[j]) * uy;
            if (u <= 0) continue;
            float q = r2 * ir * inv_h;
            float I = dt * (1 - q) * (VISC_LIN * u + VISC_QUAD * u * u) * 0.5f;
            if (I > u * 0.5f) I = u * 0.5f;
            f->vx[i] -= I * ux; f->vy[i] -= I * uy;
            f->vx[j] += I * ux; f->vy[j] += I * uy;
        }
    }
    for (int i = 0; i < n; i++) {
        f->px[i] = f->x[i];
        f->py[i] = f->y[i];
        f->x[i] += f->vx[i] * dt;
        f->y[i] += f->vy[i] * dt;
    }
    /* Double-density relaxation. Each pair's distance is found once, in the
       density pass, and reused for the push: the push moves drops a fraction
       of a pixel, and a second square root and two divides per pair were
       most of the frame on watch. */
    const float dt2 = dt * dt;
    for (int i = 0; i < n; i++) {
        float rho = 0, rhon = 0;
        float qk[FLUID_NEIGH], ux[FLUID_NEIGH], uy[FLUID_NEIGH];
        int m = f->nn[i];
        for (int k = 0; k < m; k++) {
            int j = f->nb[i][k];
            float dx = f->x[j] - f->x[i], dy = f->y[j] - f->y[i];
            float r2 = dx * dx + dy * dy;
            qk[k] = 0;
            if (r2 >= FLUID_RADIUS * FLUID_RADIUS || r2 < 1e-8f) continue;
            float ir = 1.0f / sqrtf(r2);
            float q = 1 - r2 * ir * inv_h;
            qk[k] = q;
            ux[k] = dx * ir;
            uy[k] = dy * ir;
            rho += q * q;
            rhon += q * q * q;
        }
        float P = STIFF * (rho - REST_DENSITY), Pn = STIFF_NEAR * rhon;
        float ddx = 0, ddy = 0;
        for (int k = 0; k < m; k++) {
            float q = qk[k];
            if (q <= 0) continue;
            int j = f->nb[i][k];
            float D = dt2 * (P * q + Pn * q * q) * 0.5f;
            if (D > 2.0f) D = 2.0f;               /* no drop flung across the bottle */
            float mx = D * ux[k], my = D * uy[k];
            f->x[j] += mx; f->y[j] += my;
            ddx -= mx; ddy -= my;
        }
        f->x[i] += ddx;
        f->y[i] += ddy;
    }
    /* Walls, then velocity from where each drop actually went. A drop
       stopped by a wall loses its speed into it, as water does. */
    const float x1 = f->w - WALL, y1 = f->h - WALL;
    for (int i = 0; i < n; i++) {
        if (f->x[i] < WALL) f->x[i] = WALL;
        if (f->x[i] > x1) f->x[i] = x1;
        if (f->y[i] < WALL) f->y[i] = WALL;
        if (f->y[i] > y1) f->y[i] = y1;
        f->vx[i] = (f->x[i] - f->px[i]) / dt;
        f->vy[i] = (f->y[i] - f->py[i]) / dt;
    }
}

void fluid_step(fluid_t *f, float gx, float gy, float dt)
{
    int steps = (int)(dt / SUBSTEP + 0.5f);
    if (steps < 1) steps = 1;
    if (steps > FLUID_MAX_STEPS) steps = FLUID_MAX_STEPS;
    for (int s = 0; s < steps; s++) substep(f, gx, gy, SUBSTEP);
}

static uint16_t rgb(int r, int g, int b)
{
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* A bead 7 px across -- the drops sit about that far apart at rest, so a
   still pool reads as a body of touching beads -- with a highlight up and
   left. 0 outside, 1 body, 2 highlight. */
#define BEAD 7
static const uint8_t BEAD_MASK[BEAD][BEAD] = {
    { 0, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 2, 2, 1, 1, 0 },
    { 1, 2, 2, 1, 1, 1, 1 },
    { 1, 2, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 0, 0 },
};

/* `c` a share `w` of the way to white, in RGB565. */
static uint16_t lighten(uint16_t c, float w)
{
    int r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
    return rgb((int)(r + (255 - r) * w), (int)(g + (255 - g) * w), (int)(b + (255 - b) * w));
}

void fluid_snapshot(const fluid_t *f, fluid_view_t *v)
{
    v->n = f->n;
    for (int i = 0; i < f->n; i++) {
        v->x[i] = f->x[i];
        v->y[i] = f->y[i];
        v->colour[i] = f->colour[i];
        float sp2 = f->vx[i] * f->vx[i] + f->vy[i] * f->vy[i];
        int k = (int)(sp2 * (3.0f / (500.0f * 500.0f)));
        v->speed[i] = (uint8_t)(k > 3 ? 3 : k);
    }
}

void fluid_draw(const fluid_view_t *f, canvas_t *c)
{
    const int W = c->w, H = c->h;
    uint16_t *fb = c->fb;
    memset(fb, 0, (size_t)W * H * sizeof(uint16_t));
    /* The sand's six colours (particles.c), each with a highlight, and a
       paler body for a drop moving fast -- spray from a splash. */
    static const uint16_t base[6] = { PAL_A0, PAL_A1, PAL_A2, PAL_A3, PAL_A4, PAL_A5 };
    static uint16_t body[6][4], hi[6];
    static int ready;
    if (!ready) {
        for (int k = 0; k < 6; k++) {
            for (int s2 = 0; s2 < 4; s2++) body[k][s2] = lighten(base[k], s2 * 0.15f);
            hi[k] = lighten(base[k], 0.65f);
        }
        ready = 1;
    }
    for (int i = 0; i < f->n; i++) {
        uint16_t bc = body[f->colour[i]][f->speed[i]], hc = hi[f->colour[i]];
        int x0 = (int)f->x[i] - BEAD / 2, y0 = (int)f->y[i] - BEAD / 2;
        for (int yy = 0; yy < BEAD; yy++) {
            int y = y0 + yy;
            if (y < 0 || y >= H) continue;
            uint16_t *row = fb + (size_t)y * W;
            for (int xx = 0; xx < BEAD; xx++) {
                int x = x0 + xx, m = BEAD_MASK[yy][xx];
                if (!m || x < 0 || x >= W) continue;
                row[x] = m == 2 ? hc : bc;
            }
        }
    }
}
