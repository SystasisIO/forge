#pragma once

#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

int isalnum(int value);
int isalpha(int value);
int isblank(int value);
int iscntrl(int value);
int isdigit(int value);
int isgraph(int value);
int islower(int value);
int isprint(int value);
int ispunct(int value);
int isspace(int value);
int isupper(int value);
int isxdigit(int value);
int tolower(int value);
int toupper(int value);
int tolower_l(int value, locale_t locale);
int toupper_l(int value, locale_t locale);

#ifdef __cplusplus
}
#endif
