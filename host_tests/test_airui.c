/* airui: speaker's Air page renders in each state (host_tests/renders/airui/, PPM). */
#include "airui.h"

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
    printf(s_fail ? "%d FAILED\n" : "all passed\n", s_fail);
    return s_fail != 0;
}
