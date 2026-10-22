// Base64 kodlama / çözme. Donanıma bağlı değil.
#pragma once
#include <stdint.h>
#include <stddef.h>

// n baytı kodlar. out en az 4*((n+2)/3) bayt olmalı. Yazılan karakter sayısını döner.
inline size_t b64Encode(const uint8_t *in, size_t n, char *out) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0, i = 0;
  for (; i + 2 < n; i += 3) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
    out[o++] = T[v >> 18];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63];
    out[o++] = T[v & 63];
  }
  if (i + 1 == n) {
    uint32_t v = (uint32_t)in[i] << 16;
    out[o++] = T[v >> 18];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = '=';
    out[o++] = '=';
  } else if (i + 2 == n) {
    uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
    out[o++] = T[v >> 18];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = T[(v >> 6) & 63];
    out[o++] = '=';
  }
  return o;
}

// Akış halinde çözücü. Alfabede olmayan karakterleri (ters bölü, boşluk, '=')
// atlar; bu sayede JSON içinde "\/" diye kaçışlanmış veri de doğru çözülür.
struct B64Decoder {
  uint32_t acc = 0;
  int bits = 0;
  void reset() { acc = 0; bits = 0; }

  static int value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  }

  // in[0..inLen) karakterlerini çözer, en çok outCap bayt yazar.
  // *used: tüketilen karakter sayısı. Dönen değer: yazılan bayt.
  size_t feed(const char *in, size_t inLen, uint8_t *out, size_t outCap, size_t *used) {
    size_t o = 0, i = 0;
    for (; i < inLen && o < outCap; i++) {
      int v = value((uint8_t)in[i]);
      if (v < 0) continue;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out[o++] = (uint8_t)(acc >> bits);
        acc &= (1u << bits) - 1;
      }
    }
    *used = i;
    return o;
  }
};
