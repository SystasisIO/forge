#pragma once

#include "locale_impl.h"

// The freestanding SDK has one deterministic C locale, not a thread/host locale.
// Keep the pinned libc algorithms and bind their locale macros to that descriptor.
#undef CURRENT_LOCALE
#define CURRENT_LOCALE C_LOCALE
#undef CURRENT_UTF8
#define CURRENT_UTF8 0
