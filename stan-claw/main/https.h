#ifndef HTTPS_H
#define HTTPS_H

/* One HTTPS POST, for Deepgram, Claude and ElevenLabs alike: the reply
   collected into a PSRAM buffer, or handed to a sink as it arrives. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { const char *name, *value; } https_hdr_t;
typedef bool (*https_sink_t)(const uint8_t *data, int len, void *ctx);

/* Returns the HTTP status, or -1 when it could not connect or timed out.
   With no sink, *resp is a NUL-terminated malloc'd copy of up to `max`
   bytes (the caller frees it), also for error statuses. */
int https_post(const char *url, const https_hdr_t *hdrs, int nhdrs, const char *ctype,
               const void *body, size_t len, int timeout_ms, char **resp, size_t max,
               https_sink_t sink, void *ctx);

/* A GET: https_post with no body (and no content type). */
static inline int https_get(const char *url, const https_hdr_t *hdrs, int nhdrs, int timeout_ms, char **resp, size_t max)
{
    return https_post(url, hdrs, nhdrs, NULL, NULL, 0, timeout_ms, resp, max, NULL, NULL);
}

#endif /* HTTPS_H */
