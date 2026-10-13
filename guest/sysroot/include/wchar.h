#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t wint_t;
typedef struct {
   uint32_t state[2];
} mbstate_t;

#define WEOF ((wint_t)-1)

#ifdef __cplusplus
extern "C" {
#endif

size_t mbrtowc(wchar_t* character, const char* source, size_t size, mbstate_t* state);
int mbsinit(const mbstate_t* state);

#ifdef __cplusplus
}
#endif
