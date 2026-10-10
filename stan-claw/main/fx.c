#include "fx.h"

#include <math.h>
#include <string.h>

void fx_init(fx_t *f, fx_kind_t kind)
{
    memset(f, 0, sizeof *f);
    f->kind = kind;
}

fx_kind_t fx_from_name(const char *n)
{
    if (n && strcmp(n, "hal") == 0) return FX_HAL;
    if (n && strcmp(n, "jarvis") == 0) return FX_JARVIS;
    return FX_NONE;
}

const char *fx_name(fx_kind_t k) { return k == FX_HAL ? "hal" : k == FX_JARVIS ? "jarvis" : ""; }

void fx_run(fx_t *f, int16_t *pcm, int n)
{
    if (f->kind == FX_NONE) return;
    /* HAL: 5.5 ms comb (a low metallic ring), dulled; Jarvis: 3 ms comb
       (higher), a 90 Hz ring modulator at 25 %. */
    int delay = f->kind == FX_HAL ? 88 : 48;
    float fb = f->kind == FX_HAL ? 0.42f : 0.38f;
    float wet = f->kind == FX_HAL ? 0.45f : 0.40f;
    float lp_a = f->kind == FX_HAL ? 0.55f : 1.0f;      /* 1 = no dulling */
    float ring = f->kind == FX_JARVIS ? 0.25f : 0.0f;
    const float step = 2.0f * 3.14159265f * 90.0f / 16000.0f;
    for (int i = 0; i < n; i++) {
        float x = pcm[i] / 32768.0f;
        int rd = (f->at - delay + FX_DELAY_MAX) % FX_DELAY_MAX;
        float d = f->line[rd];
        float y = x + fb * d;
        f->line[f->at] = y;
        f->at = (f->at + 1) % FX_DELAY_MAX;
        float out = (1.0f - wet) * x + wet * y;
        f->lp += lp_a * (out - f->lp);
        out = f->lp;
        if (ring > 0) {
            out = (1.0f - ring) * out + ring * out * sinf(f->phase);
            f->phase += step;
            if (f->phase > 6.2831853f) f->phase -= 6.2831853f;
        }
        out *= 0.82f;                                   /* headroom for the comb's peaks */
        if (out > 0.999f) out = 0.999f;
        if (out < -0.999f) out = -0.999f;
        pcm[i] = (int16_t)lrintf(out * 32767.0f);
    }
}
