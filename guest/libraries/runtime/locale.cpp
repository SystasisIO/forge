#include "details/locale.hxx"

#include <forge/contract/intrinsics.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

namespace {

locale_t current_locale = LC_GLOBAL_LOCALE;

locale_t c_locale() {
   return const_cast<struct __locale_struct*>(&__c_locale);
}

bool supported_locale(locale_t locale) {
   return locale == c_locale() || locale == LC_GLOBAL_LOCALE;
}

bool supported_name(const char* name) {
   return name != nullptr && (strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0 || *name == '\0');
}

} // namespace

extern "C" {

int* __errno_location(void) {
   return __forge_contract_errno();
}

locale_t newlocale(int mask, const char* name, locale_t base) {
   constexpr auto category_mask = (1 << LC_ALL) - 1;
   if ((mask != LC_ALL_MASK && (mask & ~category_mask) != 0) || !supported_name(name) ||
       (base != nullptr && base != c_locale())) {
      errno = EINVAL;
      return nullptr;
   }
   return c_locale();
}

locale_t duplocale(locale_t locale) {
   if (!supported_locale(locale)) {
      errno = EINVAL;
      return nullptr;
   }
   return c_locale();
}

void freelocale(locale_t locale) {
   if (locale != c_locale()) {
      errno = EINVAL;
   }
}

locale_t uselocale(locale_t locale) {
   const auto previous = current_locale;
   if (locale != nullptr) {
      if (!supported_locale(locale)) {
         errno = EINVAL;
         return nullptr;
      }
      current_locale = locale;
   }
   return previous;
}

char* setlocale(int category, const char* name) {
   if (category < LC_CTYPE || category > LC_ALL || (name != nullptr && !supported_name(name))) {
      errno = EINVAL;
      return nullptr;
   }
   return const_cast<char*>("C");
}

// The donor _l aliases have non-POSIX two-argument types. Bind the correct
// public three-argument ABI to the unchanged C-locale conversion primitives.
float strtof_l(const char* source, char** end, locale_t) {
   return strtof(source, end);
}

double strtod_l(const char* source, char** end, locale_t) {
   return strtod(source, end);
}

long double strtold_l(const char* source, char** end, locale_t) {
   return strtold(source, end);
}

[[noreturn]] void _Exit(int status) {
   eosio_exit(status);
   __builtin_unreachable();
}

[[noreturn]] void exit(int status) {
   __funcs_on_exit();
   _Exit(status);
}

} // extern "C"
