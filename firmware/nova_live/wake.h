// "Hey Nova" uyandırma sözü: Espressif ESP-SR WakeNet10, hazır wn10_heynova modeli (heynova_model.c).
// Yalnızca kartta derlenir (link.cpp'ye LinkHooks üzerinden bağlanır; masaüstü testinde sahtesi var).
//
// Motor yalnızca oturum kapalıyken çalışır: oturum açılırken kaldırılır, iç RAM şifreli bağlantıya
// kalsın ve Nova kendi sesiyle uyanmasın. Arduino'nun ESP_SR kütüphanesi kullanılmıyor: o, mikrofonu
// kendisi okumak ve hazır modeli her yüklemede ayrı bölüme yazdırmak istiyor. Burada ESP-SR'nin C
// arayüzü doğrudan çağrılır, ses bizim mikrofon zincirinden (16 kHz, iki mikrofonun ortalaması) gelir.
#pragma once
#include <stdint.h>
#include <stddef.h>

// 0: kapalı, 1: normal (daha az yanlış uyanma), 2: hassas (daha çok yakalar, daha sık yanlış uyanır),
// 3: çok hassas (hassasın eşiği WAKE_VERY_DROP kadar düşürülmüş; en çok yakalar, yanlış uyanma artar)
enum : int { WAKE_OFF = 0, WAKE_NORMAL = 1, WAKE_SENSITIVE = 2, WAKE_VERY = 3 };
#define WAKE_VERY_DROP 0.10f

bool wakeBegin();                                // modeli okur (firmware'in içinden); olmazsa false
bool wakeArm(bool on);                           // true: motoru kur, false: kaldır. Kuruluysa true döner
bool wakeFeed(const int16_t *pcm, size_t n);     // 16 kHz mono ses; söz duyulunca true
void wakeSetMode(int mode);                      // bir sonraki kurulumda geçerli olur
int wakeMode();
const char *wakeModeName(int mode);
