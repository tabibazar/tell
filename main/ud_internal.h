#ifndef UD_INTERNAL_H
#define UD_INTERNAL_H

/*
 * Shared between the parser core and the one-file-per-payload sections. Not
 * for callers outside usagedata: they use usagedata.h.
 */
#include "usagedata.h"

#include <stdbool.h>
#include <stdint.h>

/* One payload kind: its marker, how to reset a machine's copy, how to read
   one row, how to validate the result, how to fold every machine's copy into
   the view, and whether a machine has sent it at all (which is what makes
   the machine count in legends and freshness). `h` is NULL for the sections
   that are not per machine, and so are their `used` hooks. */
typedef struct {
    const char *marker;
    ud_kind_t kind;
    bool per_host;
    void (*begin)(usagedata_t *d, ud_host_t *h, int64_t now_us);
    void (*line)(usagedata_t *d, ud_host_t *h, const char *tag, const char *rest);
    void (*end)(usagedata_t *d, ud_host_t *h);
    void (*merge)(const usagedata_t *d, ud_view_t *out);
    bool (*used)(const ud_host_t *h);     /* has this machine sent this section? */
} ud_section_t;

extern const ud_section_t *const ud_sections[];
extern const int ud_section_count;

/* Copies up to `max` chars of a whitespace-delimited token; NULL if none. */
const char *ud_token(const char *p, char *out, int max);

/* The rest of the line, verbatim, up to `max` chars. */
void ud_text(const char *q, char *dest, int max);

/* Machines that have sent anything, in the order the view numbers them. */
bool ud_contributes(const ud_host_t *h);
int ud_host_order(const usagedata_t *d, const ud_host_t *hosts[UD_MAX_HOSTS]);

int ud_cmp_u64(const void *a, const void *b);
int ud_rank_levels(const uint64_t *vals, int n, uint8_t *level, uint64_t *scratch);
void ud_pick_window(const int32_t *starts, const int32_t *todays, int n,
                    int32_t *start, int32_t *today, int *len);

#endif /* UD_INTERNAL_H */
