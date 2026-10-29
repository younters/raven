/* vkd3d-shader built with mingw-w64 headers calls these as functions; Wine's ucrtbase import library has only the underlying exports. */
#include <stdarg.h>
#include <stddef.h>
typedef struct _iobuf FILE;
struct _stat64i32;
int __cdecl __stdio_common_vsprintf(unsigned __int64, char *, size_t, const char *, void *, va_list);
int __cdecl __stdio_common_vfprintf(unsigned __int64, FILE *, const char *, void *, va_list);
int __cdecl _fileno(FILE *);
int __cdecl _fstat64i32(int, struct _stat64i32 *);
int __cdecl vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    const int result = __stdio_common_vsprintf(2 /* standard snprintf behaviour */, buffer, size, format, NULL, args);
    return result < 0 ? -1 : result;
}
int __cdecl snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format);
    const int result = vsnprintf(buffer, size, format, args);
    va_end(args); return result;
}
int __cdecl vfprintf(FILE *file, const char *format, va_list args) { return __stdio_common_vfprintf(0, file, format, NULL, args); }
int __cdecl fprintf(FILE *file, const char *format, ...)
{
    va_list args; va_start(args, format);
    const int result = vfprintf(file, format, args);
    va_end(args); return result;
}
int __cdecl fileno(FILE *file) { return _fileno(file); }
int __cdecl fstat64i32(int descriptor, struct _stat64i32 *buffer) { return _fstat64i32(descriptor, buffer); }
int __cdecl __stdio_common_vsscanf(unsigned __int64, const char *, size_t, const char *, void *, va_list);
int __cdecl sprintf(char *buffer, const char *format, ...)
{
    va_list args; va_start(args, format);
    const int result = __stdio_common_vsprintf(1 /* legacy sprintf: no truncation */, buffer, (size_t)-1, format, NULL, args);
    va_end(args); return result < 0 ? -1 : result;
}
int __cdecl sscanf(const char *text, const char *format, ...)
{
    va_list args; va_start(args, format);
    const int result = __stdio_common_vsscanf(0, text, (size_t)-1, format, NULL, args);
    va_end(args); return result;
}
typedef struct __crt_locale_pointers *crt_locale;
double __cdecl _strtod_l(const char *, char **, crt_locale);
float __cdecl raven_strtof_l(const char *text, char **end, crt_locale locale) { return (float)_strtod_l(text, end, locale); }
/* vkd3d calls it through a dllimport slot. */
float (__cdecl *__imp_raven_strtof_l)(const char *, char **, crt_locale) = raven_strtof_l;
