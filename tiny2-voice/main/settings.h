#ifndef SETTINGS_H
#define SETTINGS_H

/*
 * tiny2-voice's settings in NVS (namespace "tiny2v"): the WiFi networks it
 * knows, the ElevenLabs key and voice, the token /say wants, the volume and
 * the speaking speed. The first boot seeds them from the build (secrets/);
 * the console changes them after. Safe from any task.
 */
#include <stdbool.h>
#include <stddef.h>

#define SET_NETS 6
#define SET_VAL  200

typedef struct { char ssid[33]; char pass[65]; } set_net_t;
typedef enum { SET_ELEVEN_KEY, SET_VOICE, SET_TOKEN, SET_VOLUME, SET_SPEED, SET_N } set_key_t;

void set_load(void);
int set_nets(set_net_t *out);
bool set_net_add(const char *ssid, const char *pass);
void set_get(set_key_t k, char *buf, size_t n);
void set_put(set_key_t k, const char *v);
bool set_has(set_key_t k);

#endif /* SETTINGS_H */
