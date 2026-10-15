#ifndef TZ_H
#define TZ_H
/* An IANA time zone name ("America/Toronto") to the POSIX TZ rule the C
   library's setenv("TZ") understands ("EST5EDT,M3.2.0,M11.1.0"); NULL for a
   name it does not know. The table is tzdb.h, made by tools/gen_tzdb.py.
   Pure: host_tests/test_sc_weather.c. */
const char *tz_posix(const char *iana);
#endif /* TZ_H */
