// Cihazın kendi yaptığı küçük HTTPS istekleri (hava durumu, oturum sonu not çıkarma).
// HTTPClient kütüphanesi eklenmez (uygulama bölümü neredeyse dolu); istek HTTP/1.0 ile gider, sunucu
// yine de parçalı (chunked) yollarsa çözülür. Şifreli bağlantı iç RAM ister: çağıran, uyandırma motorunu
// önceden kaldırmalı (linkSetNetBusy) ve oturum açıkken çağırmamalı.
#pragma once
#include <stddef.h>
#include <stdint.h>

// "Transfer-Encoding: chunked" gövdesini yerinde çözer; yeni uzunluğu döner (masaüstünde sınanır).
size_t chunkCoz(char *p, size_t len);
// Ham HTTP cevabını ayırır: durum kodu (yoksa -1), gövdenin başı ve uzunluğu. Parçalıysa çözer.
int httpAyir(char *ham, size_t len, char **govde, size_t *govdeLen);

#ifndef NOVA_HOST_TEST
// yontem: "GET" ya da "POST"; ekBasliklar: "Ad: değer\r\n" satırları (yoksa nullptr).
// Cevap buf'a yazılır (cap'i aşan kısım atılır). Durum kodunu döner; bağlantı kurulamazsa -1 (hata'da sebep).
int httpsIstek(const char *host, const char *yontem, const char *yol, const char *ekBasliklar, const char *govde,
               size_t govdeLen, char *buf, size_t cap, char **cevap, size_t *cevapLen, uint32_t zamanAsimiMs,
               char *hata, size_t hcap);
#endif
