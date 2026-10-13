#pragma once

#include <stddef.h>
#include <locale.h>

#ifdef __cplusplus
extern "C" {
#endif

void* memchr(const void* value, int byte, size_t size);
int memcmp(const void* left, const void* right, size_t size);
void* memccpy(void* destination, const void* source, int character, size_t size);
void* memcpy(void* destination, const void* source, size_t size);
void* memmove(void* destination, const void* source, size_t size);
void* memset(void* destination, int value, size_t size);

char* strcat(char* destination, const char* source);
char* strchr(const char* value, int character);
int strcmp(const char* left, const char* right);
char* strcpy(char* destination, const char* source);
size_t strcspn(const char* value, const char* rejected);
char* strerror(int error);
size_t strlen(const char* value);
char* strncat(char* destination, const char* source, size_t size);
int strncmp(const char* left, const char* right, size_t size);
char* strncpy(char* destination, const char* source, size_t size);
char* strpbrk(const char* value, const char* accepted);
char* strrchr(const char* value, int character);
size_t strspn(const char* value, const char* accepted);
char* strstr(const char* value, const char* substring);
int strcoll(const char* left, const char* right);
size_t strxfrm(char* destination, const char* source, size_t size);
int strcoll_l(const char* left, const char* right, locale_t locale);
size_t strxfrm_l(char* destination, const char* source, size_t size, locale_t locale);

#ifdef __cplusplus
}
#endif
