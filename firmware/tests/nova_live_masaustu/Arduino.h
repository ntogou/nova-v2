// Masaüstü testi için Arduino yerine geçen en küçük başlık
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string>
#ifndef PI
#define PI 3.14159265358979f
#endif
struct SerialMock {
  std::string all;
  void write(const uint8_t *b, size_t n) { all.append((const char *)b, n); fwrite(b, 1, n, stdout); }
  void print(const char *s) { write((const uint8_t *)s, strlen(s)); }
  void println(const char *s) { print(s); print("\n"); }
  void println() { print("\n"); }
  int printf(const char *fmt, ...) {
    char b[1024];
    va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    print(b); return n;
  }
};
extern SerialMock Serial;
