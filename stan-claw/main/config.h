#ifndef CONFIG_H
#define CONFIG_H

/*
 * stan-claw's settings in NVS (namespace "stanclaw"): the WiFi networks it
 * knows, the three API keys, the ElevenLabs voice, the Claude model, the
 * remote MCP server and the token its own MCP server wants. Set over the USB
 * console; never in git. First boot seeds them from the build
 * (secrets/stan-claw.env) and, on a board that ran panel1, takes panel1's
 * networks from its NVS. Safe from any task.
 */
#include <stdbool.h>
#include <stddef.h>

#define CFG_NETS 6
#define CFG_VAL 400

typedef struct {
    char ssid[33];
    char pass[65];
} cfg_net_t;

typedef enum {
    CFG_CLAUDE_KEY, CFG_DEEPGRAM_KEY, CFG_ELEVEN_KEY, CFG_VOICE, CFG_MODEL,
    CFG_MCP_URL, CFG_MCP_TOKEN, CFG_SERVE_TOKEN,
    CFG_VOICE_NAME, CFG_VOLUME, CFG_SPEED,      /* set on the settings page */
    CFG_N
} cfg_key_t;

void cfg_load(void);
int cfg_nets(cfg_net_t *out);
bool cfg_net_add(const char *ssid, const char *pass);
bool cfg_net_forget(const char *ssid);
void cfg_get(cfg_key_t k, char *buf, size_t n);
void cfg_set(cfg_key_t k, const char *v);
bool cfg_has(cfg_key_t k);

#endif /* CONFIG_H */
