/* fluid.c: pours, tilts and shakes watch's water and checks it behaves like
   water -- stays in the bottle, settles level, follows a tilt, splashes when
   shaken -- and renders frames to host_tests/renders/fluid/ to look at. */
#include "../main/fluid.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static fluid_t F;
static fluid_view_t V;
static uint16_t fb[240 * 280];

static void save(const char *name)
{
    canvas_t c = { .fb = fb, .w = 240, .h = 280, .scale = 1 };
    fluid_snapshot(&F, &V);
    fluid_draw(&V, &c);
    char path[128];
    snprintf(path, sizeof path, "host_tests/renders/fluid/%s.ppm", name);
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6 240 280 255\n");
    for (int i = 0; i < 240 * 280; i++) {
        uint16_t p = fb[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
        fwrite(rgb, 1, 3, o);
    }
    fclose(o);
}

static void stats(float *cx, float *cy, float *top, float *speed, int *bad)
{
    *cx = *cy = *speed = 0; *top = 1e9; *bad = 0;
    for (int i = 0; i < F.n; i++) {
        if (!isfinite(F.x[i]) || F.x[i] < 0 || F.x[i] >= F.w || F.y[i] < 0 || F.y[i] >= F.h) (*bad)++;
        *cx += F.x[i]; *cy += F.y[i];
        *speed += sqrtf(F.vx[i] * F.vx[i] + F.vy[i] * F.vy[i]);
        if (F.y[i] < *top) *top = F.y[i];
    }
    *cx /= F.n; *cy /= F.n; *speed /= F.n;
}

static void run(float gx, float gy, float secs)
{
    for (float t = 0; t < secs; t += 1.0f / 30) fluid_step(&F, gx, gy, 1.0f / 30);
}

int main(void)
{
    const float G = 1000;
    int n = fluid_for(240, 280);
    fluid_init(&F, n, 240, 280, 1234);
    save("0-start");
    clock_t c0 = clock();
    run(0, G, 4);
    double per_frame_ms = (clock() - c0) * 1000.0 / CLOCKS_PER_SEC / (4 * 30);
    float cx, cy, top, speed; int bad;
    stats(&cx, &cy, &top, &speed, &bad);
    save("1-settled");
    printf("%d drops; settled: centre %.0f,%.0f top %.0f speed %.1f px/s; host %.2f ms/frame\n", n, cx, cy, top, speed, per_frame_ms);
    CHECK(bad == 0, "%d drops out of the bottle", bad);
    CHECK(speed < 25, "not settled: %.1f px/s", speed);
    /* Level: the surface drops' heights spread little. */
    float hi = -1e9, lo = 1e9;
    for (int i = 0; i < F.n; i++) {
        int surface = 1;
        for (int j = 0; j < F.n && surface; j++)
            if (j != i && fabsf(F.x[j] - F.x[i]) < 5 && F.y[j] < F.y[i] - 3) surface = 0;
        if (surface && F.x[i] > 20 && F.x[i] < 220) { if (F.y[i] > hi) hi = F.y[i]; if (F.y[i] < lo) lo = F.y[i]; }
    }
    printf("surface between y %.0f and %.0f\n", lo, hi);
    CHECK(hi - lo < 22, "surface not level: %.0f px", hi - lo);
    CHECK(top > 280 * 0.35f && top < 280 * 0.85f, "fill level odd: top %.0f", top);

    /* Tilt right: the water runs right and piles against the wall. */
    run(G * 0.7f, G * 0.7f, 1.5f);
    float cx2; stats(&cx2, &cy, &top, &speed, &bad);
    save("2-tilted");
    printf("tilted: centre x %.0f (was %.0f)\n", cx2, cx);
    CHECK(cx2 > cx + 25, "did not follow the tilt");
    CHECK(bad == 0, "%d out after tilt", bad);

    /* Back level, then a shake: a hard jolt up and down. Drops fly above
       where the surface was. */
    run(0, G, 2.0f);
    float top0; stats(&cx, &cy, &top0, &speed, &bad);
    float highest = 1e9;
    for (int k = 0; k < 6; k++) {
        run(0, k % 2 ? 3.0f * G : -1.5f * G, 0.1f);
        float t; stats(&cx, &cy, &t, &speed, &bad);
        if (t < highest) highest = t;
        if (k == 3) save("3-splash");
    }
    printf("shake: surface %.0f, highest drop %.0f\n", top0, highest);
    CHECK(highest < top0 - 30, "no splash: highest %.0f vs surface %.0f", highest, top0);
    CHECK(bad == 0, "%d out after the shake", bad);
    /* Sideways slosh. */
    for (int k = 0; k < 8; k++) run(k % 2 ? 1.6f * G : -1.6f * G, G, 0.12f);
    save("4-slosh");
    run(0, G, 5);
    stats(&cx, &cy, &top, &speed, &bad);
    save("5-calm");
    printf("calm again: speed %.1f, top %.0f\n", speed, top);
    CHECK(speed < 35, "did not calm: %.1f px/s", speed);
    CHECK(bad == 0, "%d out at the end", bad);
    printf(fails ? "%d FAILED\n" : "fluid: all passed\n", fails);
    return fails != 0;
}
