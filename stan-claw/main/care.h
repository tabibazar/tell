#ifndef CARE_H
#define CARE_H

/*
 * Pure helpers for the board's own calls to stan-mcp (JSON-RPC over HTTPS,
 * via the existing HTTPS helper and a homeowner token learned from Claude's
 * reply), and for the reminders and family messages it keeps in flash.
 * Uses cJSON. Pure: host_tests/test_sc_care.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define CARE_MAX   12
#define CARE_TEXT  240

typedef struct {
    char id[48];
    long at;                 /* unix seconds UTC: when to say it (reminders) or when it was sent (messages) */
    char from[32];           /* messages: the family member's name; reminders: "" */
    char text[CARE_TEXT];
    bool done;               /* reminders: already said; messages: already shown; photos: kept on flash */
    char photo[16];          /* messages: the id of the photo it brings, "" for words alone */
} care_item_t;

typedef struct { care_item_t item[CARE_MAX]; int n; } care_list_t;

/* The JSON-RPC body for tools/call of `tool` with arguments `args_json` (an
   object, e.g. {"homeowner_token":"hw_.."}). malloc'd; NULL on bad
   args_json. id 1. */
char *care_rpc_body(const char *tool, const char *args_json);

/* The text of a JSON-RPC tools/call reply: result.content[0].text into out.
   False on error replies or junk. */
bool care_rpc_text(const char *resp, char *out, size_t n);

/* Parses the reminders tool's text: {"reminders":[{"id":"..","at":1760300000,"say":".."}, ...]}.
   Replaces `l`'s items but KEEPS done=true for ids that were already done
   in `l`. Returns count, -1 on junk. */
int care_parse_reminders(const char *text, care_list_t *l);

/* Parses the family_inbox tool's text: {"messages":[{"id":"..","from":"Reza","text":"..","at":1760300000}, ...]}.
   APPENDS messages whose id is not already in `l` (dropping the oldest when
   full). Returns how many were new, -1 on junk. */
int care_parse_messages(const char *text, care_list_t *l);

/* The reminders tool's "checkin": 1 on, 0 off, -1 junk or missing. */
int care_parse_checkin(const char *text);

/* Parses the family_photos tool's text: {"photos":[{"id":"..","from":"Reza","caption":"..","at":1760300000}, ...]}
   (newest last). Replaces `l`'s items (text = the caption), keeping done=true for ids already done.
   Returns count, -1 on junk. */
int care_parse_photos(const char *text, care_list_t *l);

/* The family_media tool's text, {"id":"..","kind":"jpeg","b64":"..."}: the decoded bytes into out
   (at most max). Returns the byte count, -1 on junk, an empty b64 or too big. */
int care_media_bytes(const char *text, unsigned char *out, int max);

/* Allowed to speak on its own now? Local minute of day within 09:00 <= m < 19:00. */
bool care_may_speak(int minute);

/* The index of the reminder to say now, or -1: not done, at <= now,
   now - at < 6 h, and care_may_speak(minute). The earliest such `at` first. */
int care_due(const care_list_t *l, long now, int minute);

/* The next moment (unix) worth waking for: the earliest not-done reminder
   with at > now; 0 if none. */
long care_next_wake(const care_list_t *l, long now);

/* The list as JSON (malloc'd) and back, for /mem (keeps done flags).
   care_list_load returns false on junk. */
char *care_list_json(const care_list_t *l);
bool care_list_load(const char *json, care_list_t *l);

#endif /* CARE_H */
