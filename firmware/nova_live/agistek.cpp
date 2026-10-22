#include "agistek.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#ifndef NOVA_HOST_TEST
#include <Arduino.h>
#include <NetworkClientSecure.h>
#endif

size_t chunkCoz(char *p, size_t len) {
  size_t r = 0, w = 0;
  while (r < len) {
    char *e = nullptr;
    unsigned long n = strtoul(p + r, &e, 16);
    if (e == p + r) break;
    const char *satirSonu = (const char *)memchr(p + r, '\n', len - r);
    if (!satirSonu) break;
    r = (size_t)(satirSonu - p) + 1;
    if (n == 0) break;
    if (r + n > len) n = len - r;                // eksik gelen son parça: olanı al
    memmove(p + w, p + r, n);
    w += n;
    r += n;
    if (r < len && p[r] == '\r') r++;
    if (r < len && p[r] == '\n') r++;
  }
  p[w] = 0;
  return w;
}

static bool basliktaVar(const char *bas, size_t basLen, const char *aranan) {
  size_t n = strlen(aranan);
  for (size_t i = 0; i + n <= basLen; i++) {
    size_t k = 0;
    while (k < n && tolower((unsigned char)bas[i + k]) == aranan[k]) k++;
    if (k == n) return true;
  }
  return false;
}

int httpAyir(char *ham, size_t len, char **govde, size_t *govdeLen) {
  *govde = ham + len;
  *govdeLen = 0;
  int kod = -1;
  if (len < 12 || strncmp(ham, "HTTP/", 5) != 0) return -1;
  const char *bosluk = (const char *)memchr(ham, ' ', len);
  if (!bosluk) return -1;
  kod = atoi(bosluk + 1);
  char *ayrac = strstr(ham, "\r\n\r\n");
  if (!ayrac) return kod;
  size_t basLen = (size_t)(ayrac - ham);
  char *g = ayrac + 4;
  size_t gl = len - (size_t)(g - ham);
  if (basliktaVar(ham, basLen, "transfer-encoding: chunked")) gl = chunkCoz(g, gl);
  *govde = g;
  *govdeLen = gl;
  return kod;
}

#ifndef NOVA_HOST_TEST
int httpsIstek(const char *host, const char *yontem, const char *yol, const char *ekBasliklar, const char *govde,
               size_t govdeLen, char *buf, size_t cap, char **cevap, size_t *cevapLen, uint32_t zamanAsimiMs,
               char *hata, size_t hcap) {
  NetworkClientSecure c;
  c.useBuiltinCACertBundle();
  c.setHandshakeTimeout(10);
  if (!c.connect(host, 443, 10000)) {
    char e[100] = "";
    c.lastError(e, sizeof(e));
    snprintf(hata, hcap, "bağlanılamadı%s%s", e[0] ? ": " : "", e);
    return -1;
  }
  c.printf("%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: nova\r\nConnection: close\r\n", yontem, yol, host);
  if (ekBasliklar) c.print(ekBasliklar);
  if (govde) c.printf("Content-Length: %u\r\n", (unsigned)govdeLen);
  c.print("\r\n");
  for (size_t o = 0; govde && o < govdeLen;) {
    size_t w = c.write((const uint8_t *)govde + o, govdeLen - o > 1024 ? 1024 : govdeLen - o);
    if (!w) { c.stop(); snprintf(hata, hcap, "istek gönderilemedi"); return -1; }
    o += w;
  }
  size_t n = 0;
  uint32_t t0 = millis();
  while ((c.connected() || c.available()) && millis() - t0 < zamanAsimiMs) {
    if (n >= cap - 1) { uint8_t at[256]; if (c.read(at, sizeof(at)) <= 0) delay(5); continue; }
    int k = c.read((uint8_t *)buf + n, cap - 1 - n);
    if (k > 0) n += (size_t)k;
    else delay(5);
  }
  c.stop();
  buf[n] = 0;
  int kod = httpAyir(buf, n, cevap, cevapLen);
  if (kod < 0) snprintf(hata, hcap, "cevap gelmedi (%u bayt)", (unsigned)n);
  return kod;
}
#endif
