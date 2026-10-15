#include "tz.h"

#include <string.h>

#include "tzdb.h"

const char *tz_posix(const char *iana)
{
    if (!iana || !iana[0]) return NULL;
    int lo = 0, hi = (int)(sizeof TZDB / sizeof TZDB[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(iana, TZDB[mid].iana);
        if (c == 0) return TZDB[mid].posix;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}
