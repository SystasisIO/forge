#pragma once

#include <stdarg.h>
#include <stddef.h>

typedef struct __forge_contract_file FILE;
typedef long fpos_t;

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifdef __cplusplus
extern "C" {
#endif

extern FILE* const stdin;
extern FILE* const stdout;
extern FILE* const stderr;

void clearerr(FILE* stream);
int fclose(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
int fflush(FILE* stream);
int fgetc(FILE* stream);
char* fgets(char* buffer, int size, FILE* stream);
FILE* fopen(const char* path, const char* mode);
int fprintf(FILE* stream, const char* format, ...);
int fputc(int character, FILE* stream);
int fputs(const char* value, FILE* stream);
size_t fread(void* buffer, size_t size, size_t count, FILE* stream);
FILE* freopen(const char* path, const char* mode, FILE* stream);
int fscanf(FILE* stream, const char* format, ...);
int fseek(FILE* stream, long offset, int origin);
long ftell(FILE* stream);
size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
int getc(FILE* stream);
int getchar(void);
void perror(const char* message);
int printf(const char* format, ...);
int putc(int character, FILE* stream);
int putchar(int character);
int puts(const char* value);
int remove(const char* path);
int rename(const char* old_path, const char* new_path);
void rewind(FILE* stream);
int scanf(const char* format, ...);
void setbuf(FILE* stream, char* buffer);
int setvbuf(FILE* stream, char* buffer, int mode, size_t size);
int snprintf(char* buffer, size_t size, const char* format, ...);
int sprintf(char* buffer, const char* format, ...);
int sscanf(const char* buffer, const char* format, ...);
FILE* tmpfile(void);
char* tmpnam(char* buffer);
int ungetc(int character, FILE* stream);
int vfprintf(FILE* stream, const char* format, va_list arguments);
int vfscanf(FILE* stream, const char* format, va_list arguments);
int vprintf(const char* format, va_list arguments);
int vscanf(const char* format, va_list arguments);
int vsnprintf(char* buffer, size_t size, const char* format, va_list arguments);
int vsprintf(char* buffer, const char* format, va_list arguments);
int vasprintf(char** buffer, const char* format, va_list arguments);
int vsscanf(const char* buffer, const char* format, va_list arguments);

#ifdef __cplusplus
}
#endif
