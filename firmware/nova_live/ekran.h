// Nova ekranının çizimi (A · Bakır Çekirdek tasarımı, 20 Ekim 2026'da seçildi).
//
// Donanımdan bağımsızdır: 160x128 RGB565 bir tampona çizer, masaüstünde de çalışır (önizleme ve test).
// ui.cpp her karede veriyi toplar, ekranCiz() ile tampona çizer ve yalnızca değişen satırları ekrana yollar.
// Kenarlar yumuşatılmıştır: halkalar ve yazılar arka planla karıştırılarak çizilir.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace ekran {

const int GEN = 160, YUK = 128;

enum Durum : uint8_t { ACILIS, WIFI, BAGLANIYOR, UYKU, DINLIYOR, DUSUNUYOR, KONUSUYOR, GUNCELLEME, PIL_BITTI };

struct Veri {
  Durum durum = UYKU;
  float t = 0;               // saniye (animasyon saati)
  float mik = 0, ses = 0;    // 0-1: mikrofon ve Nova'nın sesi (süzülmüş)
  int pil = -1;              // %; -1 = ölçüm yok
  bool wifi = false;
  int rssi = -100;
  int saat = -1, dakika = 0; // -1 = saat bilinmiyor
  int gun = 0, ay = 0, haftaGunu = 0;   // haftaGunu 0 = pazar
  const char *hava = "";     // "10° kapalı"
  const char *altyazi = "";  // Nova'nın sözü (UTF-8)
  int vurguBas = -1;         // altyazının bu bayttan sonrası (son gelen parça) kehribar
  const char *kullanici = "";// dinlerken: söylediğin; konuşurken: önceki sözün
  const char *etkinlik = ""; // düşünürken: "web'de arıyor…" gibi
  int etkinlikSn = -1;       // arka modelin süresi (sn); -1 = gösterme
  const char *bildirim = ""; // uykudayken kısa süre görünen not ("Mikrofon tamam." gibi)
  bool bootIpucu = false;    // uyandırma kelimesi kapalı: "BOOT'a bas" yazar
  int ota = 0;               // %
};

// tampon: GEN*YUK uint16_t, RGB565 (yerel bayt sırası)
void ciz(uint16_t *tampon, const Veri &v);

// ---- yardımcılar (testte de kullanılır)
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b);
// UTF-8 metnin yazı tipindeki genişliği (piksel). font: 0 saat, 1 yazı, 2 etiket, 3 küçük
int yaziGenislik(int font, const char *s, int aralik = 0, int bayt = -1);

// ---- yazı tamponu (ui.cpp kullanır; masaüstünde sınanır)
// UTF-8 yazıyı ekrana uygun hale getirir: satır sonları boşluk olur, art arda boşluklar teke iner,
// yazı tipinde olmayan simgeler (emoji vb.) atılır, Türkçe harfler kalır. bosBasla: önceki harf boşluk sayılsın.
size_t temizle(const char *src, char *out, size_t cap, bool bosBasla);
// buf'a (en çok maxLen bayt) ekler; dolarsa baştan kelime sınırında kırpar. vurgu: eklenen parçanın
// başı (bayt) buraya yazılır. gecici: temizlenmiş yazı için çalışma alanı.
void ekle(char *buf, size_t maxLen, const char *src, int *vurgu, char *gecici, size_t gcap);

}  // namespace ekran
