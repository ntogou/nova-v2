// Büyük çalışma tamponları için bellek: önce PSRAM, yoksa iç RAM.
// Sabit diziler (static int16_t buf[...]) iç RAM'de yer kaplar; oturum sırasında iç RAM şifreli
// bağlantıya, WiFi'ye ve (Aşama 3'te) yankı silmeye lazım. Bu yüzden büyük tamponlar ilk
// kullanıldıklarında buradan bir kez alınır ve bırakılmaz.
#pragma once
#include <stddef.h>
#include <stdlib.h>
#ifndef NOVA_HOST_TEST
#include "esp_heap_caps.h"
#endif

static inline void *psAlloc(size_t n) {
#ifndef NOVA_HOST_TEST
  void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return calloc(1, n);
}

// Tampon henüz alınmadıysa alır. Alınamazsa nullptr kalır (çağıran işi atlar).
template <typename T>
static inline T *psLazy(T *&p, size_t count) {
  if (!p) p = (T *)psAlloc(count * sizeof(T));
  return p;
}
