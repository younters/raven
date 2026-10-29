// SPDX-License-Identifier: MIT
#pragma once
#include <cerrno>
#include <cmath>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>

// Windows WCHAR and long are 16 and 32 bits respectively, even in a 64-bit DLL.
// Darwin wchar_t and long cannot be used as the import implementations.
namespace RavenCRT {
size_t length(const char16_t *s) { const auto *end = s; while (*end) ++end; return end-s; }
char16_t *copy(char16_t *out, const char16_t *in) {
  auto *result = out; while ((*out++ = *in++)) {} return result;
}
int compare_n(const char16_t *a, const char16_t *b, size_t n) {
  while (n--) { if (*a != *b) return *a < *b ? -1 : 1; if (!*a) break; ++a; ++b; } return 0;
}
int compare(const char16_t *a, const char16_t *b) { return compare_n(a,b,SIZE_MAX); }
const char16_t *find(const char16_t *s, uint16_t c) {
  do { if (*s == c) return s; } while (*s++); return nullptr;
}
// Parse independently to preserve end-pointer semantics on Windows UTF-16 input.
uint64_t parse(const char16_t *s, char16_t **end, int base, bool is_signed) {
  const auto *original = s;
  if (end) *end = const_cast<char16_t *>(s);
  if (base && (base < 2 || base > 36)) { errno = EINVAL; return 0; }
  while (*s == ' ' || (*s >= '\t' && *s <= '\r')) ++s;
  bool negative = *s == '-'; if (*s == '-' || *s == '+') ++s;
  auto digit = [](uint16_t c) -> unsigned { if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'z') return c-'a'+10;
    if (c >= 'A' && c <= 'Z') return c-'A'+10; return 36; };
  if ((!base || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && digit(s[2]) < 16) { base = 16; s += 2; }
  if (!base) base = *s == '0' ? 8 : 10;
  uint64_t limit = is_signed ? (negative ? 2147483648ull : 2147483647ull) : UINT32_MAX;
  uint64_t value = 0; bool overflow = false, any = false;
  while (digit(*s) < unsigned(base)) {
    auto d = digit(*s++); any = true;
    if (value > (limit-d)/base) overflow = true;
    else value = value*base+d;
  }
  if (end) *end = const_cast<char16_t *>(any ? s : original);
  if (overflow) { errno = ERANGE; return is_signed && negative ? uint64_t(int64_t(INT32_MIN)) : limit; }
  if (is_signed) return uint64_t(negative ? -int64_t(value) : int64_t(value));
  return uint32_t(negative ? -value : value);
}
int32_t wide_long(const char16_t *s, char16_t **end, int base) { return int32_t(parse(s,end,base,true)); }
uint32_t wide_ulong(const char16_t *s, char16_t **end, int base) { return uint32_t(parse(s,end,base,false)); }
int32_t ansi_long(const char *s, char **end, int base) {
  auto value = strtoll(s,end,base);
  if (value > INT32_MAX) { errno = ERANGE; return INT32_MAX; }
  if (value < INT32_MIN) { errno = ERANGE; return INT32_MIN; }
  return int32_t(value);
}
uint32_t ansi_ulong(const char *s, char **end, int base) {
  // strtoull applies the sign at 64 bits. Parse sign separately for Windows width.
  auto *p = s; while (isspace(static_cast<unsigned char>(*p))) ++p;
  bool negative = *p == '-'; if (*p == '-' || *p == '+') ++p;
  char *last; auto value = strtoull(p,&last,base);
  if (end) *end = last == p ? const_cast<char *>(s) : last;
  if (value > UINT32_MAX) { errno = ERANGE; return UINT32_MAX; }
  return uint32_t(negative ? -value : value);
}
int *error_pointer() { return &errno; }
void *resolve(const char *name) {
#define CRT(n, f) if (!strcmp(name, n)) return reinterpret_cast<void *>(f)
  CRT("_errno", error_pointer);
  CRT("memchr", static_cast<const void *(*)(const void *, int, size_t)>(&memchr));
  CRT("memcpy", memcpy); CRT("memmove", memmove); CRT("memset", memset); CRT("memcmp", memcmp);
  CRT("strchr", static_cast<const char *(*)(const char *, int)>(&strchr));
  CRT("strlen", strlen); CRT("strcmp", strcmp); CRT("strncmp", strncmp);
  CRT("strtol", ansi_long); CRT("strtoul", ansi_ulong);
  CRT("isalnum", isalnum); CRT("isspace", isspace); CRT("isxdigit", isxdigit);
  CRT("tolower", tolower); CRT("toupper", toupper);
  CRT("cos", static_cast<double (*)(double)>(&cos));
  CRT("sin", static_cast<double (*)(double)>(&sin));
  CRT("tan", static_cast<double (*)(double)>(&tan));
  CRT("atan2", static_cast<double (*)(double, double)>(&atan2));
  CRT("wcslen", length); CRT("wcscpy", copy); CRT("wcscmp", compare);
  CRT("wcsncmp", compare_n); CRT("wcschr", find); CRT("wcstol", wide_long); CRT("wcstoul", wide_ulong);
#undef CRT
  return nullptr;
}
}
