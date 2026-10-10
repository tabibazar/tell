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

void clarity_init(clarity_t *c, bool on)
{
    memset(c, 0, sizeof *c);
    c->on = on;
    /* RBJ cookbook high shelf, S=1, +6 dB above a 2.5 kHz corner at 16 kHz;
       no M_PI in strict C11, so a local literal. */
    const float pi = 3.14159265f, fs = 16000.0f, f0 = 2500.0f, db = 6.0f, s = 1.0f;
    float a = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * pi * f0 / fs;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / 2.0f * sqrtf((a + 1.0f / a) * (1.0f / s - 1.0f) + 2.0f);
    float sqrtA = sqrtf(a);
    float b0 = a * ((a + 1) + (a - 1) * cosw0 + 2 * sqrtA * alpha);
    float b1 = -2 * a * ((a - 1) + (a + 1) * cosw0);
    float b2 = a * ((a + 1) + (a - 1) * cosw0 - 2 * sqrtA * alpha);
    float a0 = (a + 1) - (a - 1) * cosw0 + 2 * sqrtA * alpha;
    float a1 = 2 * ((a - 1) - (a + 1) * cosw0);
    float a2 = (a + 1) - (a - 1) * cosw0 - 2 * sqrtA * alpha;
    c->b0 = b0 / a0; c->b1 = b1 / a0; c->b2 = b2 / a0; c->a1 = a1 / a0; c->a2 = a2 / a0;
}

void clarity_run(clarity_t *c, int16_t *pcm, int n)
{
    if (!c->on) return;
    const float pregain = 0.6f;          /* -4.4 dB headroom so the boosted highs don't wrap */
    for (int i = 0; i < n; i++) {
        float x = pcm[i] / 32768.0f * pregain;
        float y = c->b0 * x + c->b1 * c->x1 + c->b2 * c->x2 - c->a1 * c->y1 - c->a2 * c->y2;
        c->x2 = c->x1; c->x1 = x;
        c->y2 = c->y1; c->y1 = y;
        if (y > 0.999f) y = 0.999f;
        if (y < -0.999f) y = -0.999f;
        pcm[i] = (int16_t)lrintf(y * 32767.0f);
    }
}
