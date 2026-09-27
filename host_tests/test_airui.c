/* airui: speaker's Air page renders in each state (host_tests/renders/airui/, PPM). */
#include "airui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static uint16_t s_fb[AIRUI_WIDTH * AIRUI_HEIGHT];
static int s_fail;

static void render(const char *name, const airui_t *s)
{
    canvas_t c;
    canvas_init(&c, s_fb, AIRUI_WIDTH, AIRUI_HEIGHT, 1);
    airui_draw(&c, s);
    mkdir("renders", 0755);
    mkdir("renders/airui", 0755);
    char path[128];
    snprintf(path, sizeof path, "renders/airui/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { s_fail++; return; }
    fprintf(f, "P6 %d %d 255\n", AIRUI_WIDTH, AIRUI_HEIGHT);
    for (int i = 0; i < AIRUI_WIDTH * AIRUI_HEIGHT; i++) {
        uint16_t p = s_fb[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 11) * 255 / 31), (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void render_fb(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "renders/airui/%s.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { s_fail++; return; }
    fprintf(f, "P6 %d %d 255\n", AIRUI_WIDTH, AIRUI_HEIGHT);
    for (int i = 0; i < AIRUI_WIDTH * AIRUI_HEIGHT; i++) {
        uint16_t p = s_fb[i];
        unsigned char rgb[3] = { (unsigned char)((p >> 11) * 255 / 31), (unsigned char)(((p >> 5) & 63) * 255 / 63),
                                 (unsigned char)((p & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(void)
{
    airui_t s;
    memset(&s, 0, sizeof s);
    s.have_sensor = true;
    s.have_voc = s.have_eco2 = s.have_temp = s.have_rh = true;
    s.voc_ppb = 45; s.eco2_ppm = 440; s.temp_c = 23.4f; s.rh = 41.0f;
    s.voc_state = ENVS_OK; s.eco2_state = ENVS_OK; s.voc_trend = ENVS_STEADY;
    render("good", &s);
    s.voc_ppb = 260; s.eco2_ppm = 870; s.voc_state = ENVS_FAIR; s.eco2_state = ENVS_FAIR; s.voc_trend = ENVS_RISING;
    render("fair", &s);
    s.voc_ppb = 1340; s.eco2_ppm = 1180; s.voc_state = ENVS_POOR; s.eco2_state = ENVS_POOR;
    render("poor", &s);
    s.warming = true; s.warm_minutes = 2;
    render("warming", &s);
    memset(&s, 0, sizeof s);
    render("no_sensor", &s);

    /* The 24 hours: clean air with a dinner spike into FAIR and a POOR burst. */
    static airui_day_t d;
    d.have_sensor = true;
    d.now_slot = 14 * 12 + 6;               /* 14:30 */
    for (int k = 0; k < AIRUI_SLOTS; k++) {
        float base = 40.0f + 10.0f * sinf((float)k / 20.0f);
        d.voc[k] = base;
        d.eco2[k] = 420.0f + base;
        if (k > 40 && k < 70) { d.voc[k] = 300.0f + (float)(k - 40) * 12.0f; d.eco2[k] = 820.0f + (float)(k - 40) * 8.0f; }
        if (k > 200 && k < 215) { d.voc[k] = 900.0f; d.eco2[k] = 1100.0f; }
        if (k > 120 && k < 132) d.voc[k] = d.eco2[k] = NAN;
    }
    canvas_t c;
    canvas_init(&c, s_fb, AIRUI_WIDTH, AIRUI_HEIGHT, 1);
    airui_draw_day(&c, &d);
    render_fb("day");

    static airui_week_t w;
    w.have_sensor = true;
    static const char *const dn[7] = { "Sa", "Su", "Mo", "Tu", "We", "Th", "Fr" };
    for (int i = 0; i < 7; i++) { w.day[i][0] = dn[i][0]; w.day[i][1] = dn[i][1]; }
    for (int i = 0; i < 7; i++)
        for (int h = 0; h < 24; h++)
            w.cell[i][h] = i < 2 && h < 12 ? AIRUI_CELL_NONE : (h >= 18 && h <= 20) ? AIRUI_CELL_FAIR
                         : (i == 5 && h >= 18 && h <= 19) ? AIRUI_CELL_POOR : AIRUI_CELL_OK;
    w.cell[5][18] = w.cell[5][19] = AIRUI_CELL_POOR;
    for (int h = 15; h < 24; h++) w.cell[6][h] = AIRUI_CELL_FUTURE;
    w.poor_hours = 2;
    w.fair_hours = 14;
    airui_draw_week(&c, &w);
    render_fb("week");
    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
