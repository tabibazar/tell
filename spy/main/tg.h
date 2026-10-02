#ifndef TG_H
#define TG_H

/*
 * Telegram Bot API over HTTPS, for spy. The token and the one chat spy talks
 * to come from spy_secrets.h, which the build writes from the gitignored
 * secrets/spy-telegram.env. The token is part of every URL, so URLs are never
 * logged.
 */
#include <stdbool.h>
#include <stdint.h>

/* The build had a token and a chat id. */
bool tg_configured(void);

/* sendMessage, plain text. */
bool tg_send_text(const char *text);

/* A file from the card as multipart: method "sendPhoto" with field "photo",
   "sendVideo" with "video", "sendDocument" with "document". `extra` is more
   form fields as "name=value\n" lines (width, height, duration), or NULL. */
bool tg_send_file(const char *method, const char *field, const char *path,
                  const char *mime, const char *caption, const char *extra);

/* One getUpdates long poll of up to `timeout_s`. Each new text message from
   our chat goes to `on_text` with its send time (Unix). `*offset` is the
   next update id to ask for, kept by the caller. False on a network error. */
bool tg_poll(int64_t *offset, int timeout_s, void (*on_text)(const char *text, int64_t date));

#endif /* TG_H */
