#pragma once

#include <stddef.h>
#include <locale.h>

typedef long long time_t;
typedef long clock_t;

struct tm {
   int tm_sec;
   int tm_min;
   int tm_hour;
   int tm_mday;
   int tm_mon;
   int tm_year;
   int tm_wday;
   int tm_yday;
   int tm_isdst;
   long __tm_gmtoff;
   const char* __tm_zone;
};

#define CLOCKS_PER_SEC 1000000L

#ifdef __cplusplus
extern "C" {
#endif

size_t strftime(char* buffer, size_t size, const char* format, const struct tm* time);
size_t strftime_l(char* buffer, size_t size, const char* format, const struct tm* time, locale_t locale);

#ifdef __cplusplus
}
#endif
