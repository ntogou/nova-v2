// Hava durumu: Open-Meteo'dan (anahtarsız, ücretsiz) şu anki durum ve üç günlük tahmin.
// Cihaz boştayken çekilir (nova_live.ino, ağ görevi); oturum açılınca kısa bir özet talimata eklenir,
// böylece "hava nasıl" sorusu web araması (10-20 sn) olmadan cevaplanır.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Open-Meteo cevabını Türkçe özete çevirir (masaüstünde sınanır). false: cevap anlaşılmadı.
bool havaCoz(const char *json, size_t len, const char *yer, char *out, size_t cap);

#ifndef NOVA_HOST_TEST
// HTTPS ile çeker ve özeti saklar (birkaç saniye sürer; çağıran uyandırma motorunu önceden kaldırır).
bool havaCek(char *hata, size_t cap);
#endif
// Saklanan özet; hiç alınmadıysa ya da eskiyse (HAVA_ESKI_MS) 0 döner.
size_t havaYazi(char *out, size_t cap, uint32_t simdiMs);
// Ekranın alt satırı için kısa hali: "18° parçalı bulut". Özet yoksa ya da eskiyse 0 döner.
size_t havaKisa(char *out, size_t cap, uint32_t simdiMs);
uint32_t havaAlindiMs();      // son başarılı çekiş (millis); 0 = hiç
void havaSakla(const char *ozet, uint32_t simdiMs);
