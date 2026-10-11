#ifndef MEMORY_H
#define MEMORY_H

/*
 * stan-claw's memory, on its own flash: a 2 MB FAT partition ("memory",
 * wear-levelled) mounted at /mem. It keeps
 *   convo.jsn  the conversation in progress and the day it belongs to
 *              (claude_day: it lasts until 04:00 the next day)
 *   facts.txt  what it has learnt about the person, a fact a line
 *   log.txt    every exchange, timestamped; at 512 KB it becomes log.old
 *   remind.jsn the reminders it will say, and which it has said
 *   msgs.jsn   messages from family, and which it has shown
 * so neither deep sleep nor a restart forgets.
 */
#include <stdbool.h>
#include "claude_msg.h"
#include "esp_err.h"

esp_err_t memory_init(void);
bool memory_load_history(claude_history_t *h, int *day);
void memory_save_history(const claude_history_t *h, int day);
void memory_load_facts(claude_facts_t *f);
void memory_save_facts(const claude_facts_t *f);
void memory_log(const char *question, const char *answer);
void memory_print_log(int last);          /* the last exchanges, to the console */
void memory_forget(void);                 /* wipe the conversation and the facts (the log stays) */

/* Any other small file in /mem (8.3 names): read (malloc'd, NULL if none) and
   written whole, safely. remind.jsn and msgs.jsn keep the care lists. */
char *memory_read(const char *name, size_t max);
void memory_write(const char *name, const char *text);

/* The same for bytes (photos, recorded phrases): read returns the length, -1 if none,
   into a malloc'd PSRAM buffer; remove deletes one. */
int memory_read_bin(const char *name, unsigned char **out, size_t max);
bool memory_write_bin(const char *name, const void *data, size_t len);
void memory_remove(const char *name);

#endif /* MEMORY_H */
