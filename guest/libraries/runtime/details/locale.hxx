#pragma once

#include <locale.h>
#include <stddef.h>
#include <time.h>

// The pinned musl locale descriptor is private; all supported categories are C.
struct __locale_map;
struct __locale_struct {
   const struct __locale_map* volatile cat[6];
};

extern "C" {
extern const struct __locale_struct __c_locale;
int* __errno_location(void);
void __funcs_on_exit(void);
size_t __strftime_l(char* buffer, size_t size, const char* format, const struct tm* time, locale_t locale);
}
