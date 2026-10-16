#ifndef CARE_NET_H
#define CARE_NET_H

/*
 * The board's own calls to stan-mcp, for the care features: the reminders it
 * says on its own and the messages family send to its screen (tools
 * `reminders` and `family_inbox`, with the homeowner token it learnt when
 * the person signed in), and word back that a message was heard (the hidden
 * tool `message_heard`, so the office or family see it was). Both lists live in /mem, so deep sleep forgets
 * neither what is coming nor what was already said. One task (the care task)
 * changes them; others only read.
 */
#include <stdbool.h>

#include "care.h"

void care_net_load(void);                  /* the lists from /mem (call once at boot, after memory_init) */
void care_net_save(void);
bool care_net_ready(void);                 /* the MCP URL, its token and a homeowner token are all set */
bool care_net_refresh_reminders(void);     /* network: the next 8 days' reminders, keeping what was said */
int care_net_check_inbox(void);            /* network: new messages, -1 on failure */
bool care_net_heard(const char *id);       /* network: message `id` was shown and read aloud (never mid-conversation) */
care_list_t *care_reminders(void);
care_list_t *care_messages(void);
int care_next_message(void);               /* the oldest message not shown yet, or -1 */
bool care_checkin_on(void);                /* the homeowner agreed to the morning check-in (from the reminders call) */
bool care_net_checkin_missed(const char *asked);   /* network: tell family nobody answered */
int care_net_sync_photos(void);            /* network: the photo frame's list, fetching new JPEGs into /mem; -1 failure */
care_list_t *care_photos(void);            /* done = on flash */
int care_photo_load(const char *id, unsigned char **jpeg);   /* a photo from /mem (malloc'd), its length or -1 */
void care_net_print(long now);             /* the console's `care` */

#endif /* CARE_NET_H */
