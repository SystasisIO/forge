#pragma once

// Only runtime-owned objects/tests see the pinned private FILE layout.
extern "C" {
#include "stdio_impl.h"
}
