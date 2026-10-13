#pragma once

// libc++'s supported musl configuration obtains mbstate_t through this header.
// The public guest C header is the single owner of that layout.
#include <wchar.h>
