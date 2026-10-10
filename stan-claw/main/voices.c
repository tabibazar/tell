#include "voices.h"

#include <string.h>

const voice_t VOICES[VOICES_N] = {
    { "CwhRBWXzGAHq8TQ4Fs17", "Roger",   "Laid-back, casual",          "male, american",        FX_NONE },
    { "nPczCjzI2devNBz1zQrb", "Brian",   "Deep, resonant, comforting", "male, american",        FX_NONE },
    { "IKne3meq5aSn9XLyUdCD", "Charlie", "Deep, confident, energetic", "male, australian",      FX_NONE },
    { "pqHfZKP75CvOlQylNhV4", "Bill",    "Wise, mature, balanced",     "male, american, older", FX_NONE },
    { "EXAVITQu4vr4xnSDxMaL", "Sarah",   "Mature, reassuring",         "female, american",      FX_NONE },
    { "Xb7hH8MSUJpSbSDYk0k2", "Alice",   "Clear, engaging",            "female, british",       FX_NONE },
    { "XrExE9yKIg1WjnnlVkGX", "Matilda", "Knowledgeable, professional", "female, american",     FX_NONE },
    { "cgSgspJ2msm6clMCkdW9", "Jessica", "Playful, bright, warm",      "female, american",      FX_NONE },
    { "cjVigY5qzO86Huf0OWal", "Hal",     "Calm, unhurried, metallic",  "robotic",               FX_HAL },
    { "onwK4e9ZLuTAKqWW03F9", "Jarvis",  "Crisp, British, synthetic",  "robotic",               FX_JARVIS },
};

int voices_find(const char *id, const char *fx_name)
{
    fx_kind_t fx = fx_from_name(fx_name);
    for (int i = 0; i < VOICES_N; i++)
        if (id && strcmp(VOICES[i].id, id) == 0 && VOICES[i].fx == fx) return i;
    return -1;
}
