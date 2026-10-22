// Ekran çiziminin ve yazı tamponunun masaüstü testi (A · Bakır Çekirdek, 2.8.0)
// Derleme: build.sh (AddressSanitizer ile; tampon dışına yazma hemen yakalanır)
#include "ekran.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  !! BASARISIZ (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool gecerliUtf8(const char *s) {
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) return false;
    for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return false;
    p += n;
  }
  return true;
}

static int farkli(const uint16_t *t, int x0, int y0, int x1, int y1) {   // zeminden farklı piksel sayısı
  int n = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) n += t[y * ekran::GEN + x] != t[0];
  return n;
}

int main() {
  char g[1024];
  printf("== 1) temizle: boşluklar, satır sonları, emoji, Türkçe harfler\n");
  {
    char o[200];
    ekran::temizle("  Merhaba\n\ndünya\t !  ", o, sizeof(o), true);
    CHECK(!strcmp(o, "Merhaba dünya ! "), "[%s]", o);
    ekran::temizle("Çok güzel 😄 şey… “tamam” – ok", o, sizeof(o), false);
    CHECK(!strcmp(o, "Çok güzel şey… “tamam” – ok"), "[%s]", o);
    ekran::temizle("a\xC5", o, sizeof(o), false);              // yarım harf atılır
    CHECK(!strcmp(o, "a"), "[%s]", o);
    ekran::temizle("x\x80y\xFFz", o, sizeof(o), false);
    CHECK(!strcmp(o, "xyz"), "[%s]", o);
    ekran::temizle("ğğğğğğğğğğ", o, 9, false);                  // küçük tamponda harf bölünmez
    CHECK(gecerliUtf8(o) && strlen(o) <= 8, "[%s]", o);
    ekran::temizle(nullptr, o, sizeof(o), false);
    CHECK(o[0] == 0, "null");
  }

  printf("== 2) ekle: vurgu son parçada, dolunca baştan kelime sınırında kırpılır\n");
  {
    char b[101] = "";
    int v = -1;
    ekran::ekle(b, 100, "Keşke.", &v, g, sizeof(g));
    CHECK(v == 0 && !strcmp(b, "Keşke."), "ilk: %d [%s]", v, b);
    ekran::ekle(b, 100, " Ama biri uyuyacaksa", &v, g, sizeof(g));
    CHECK(v == (int)strlen("Keşke.") && !strcmp(b + v, " Ama biri uyuyacaksa"), "ikinci: %d [%s]", v, b);
    ekran::ekle(b, 100, " o sensin.", &v, g, sizeof(g));
    for (int i = 0; i < 40; i++) {
      ekran::ekle(b, 100, " Ben zaten nöbetteyim, şşş.", &v, g, sizeof(g));
      CHECK(strlen(b) <= 100 && gecerliUtf8(b), "tur %d: %zu", i, strlen(b));
      CHECK(v >= 0 && v <= (int)strlen(b) && !strcmp(b + v, " Ben zaten nöbetteyim, şşş."), "vurgu tur %d: %d", i, v);
    }
    CHECK(b[0] != ' ', "kirpma bosluk birakti [%s]", b);
    // tek parça tampondan uzun
    std::string uzun;
    for (int i = 0; i < 60; i++) uzun += "çğış ";
    ekran::ekle(b, 100, uzun.c_str(), &v, g, sizeof(g));
    CHECK(strlen(b) <= 100 && gecerliUtf8(b) && v == 0, "uzun: %zu %d", strlen(b), v);
    // ardışık boşluk iki parça arasında da teke iner
    char c[50] = "";
    ekran::ekle(c, 49, "bir ", nullptr, g, sizeof(g));
    ekran::ekle(c, 49, " iki", nullptr, g, sizeof(g));
    CHECK(!strcmp(c, "bir iki"), "[%s]", c);
  }

  printf("== 3) bütün durumlar çizilir; uzun ve tuhaf yazılar taşmaz\n");
  {
    std::vector<uint16_t> t(ekran::GEN * ekran::YUK + 64, 0xBEEF);   // sondaki 64 nöbetçi
    std::string cokUzun;
    for (int i = 0; i < 40; i++) cokUzun += "Uzuuuuuuuuuuuuuuuuuuuuuuuuuuunkelime ";
    const char *yazilar[] = {"", "Kısa.", cokUzun.c_str(), "😄😄😄", "ğĞıİşŞçÇöÖüÜâîû…·°–’"};
    ekran::Durum durumlar[] = {ekran::ACILIS, ekran::WIFI, ekran::BAGLANIYOR, ekran::UYKU, ekran::DINLIYOR,
                               ekran::DUSUNUYOR, ekran::KONUSUYOR, ekran::GUNCELLEME, ekran::PIL_BITTI};
    int kare = 0;
    double enUzunMs = 0;
    for (auto d : durumlar)
      for (auto y : yazilar)
        for (int k = 0; k < 4; k++) {
          ekran::Veri v;
          v.durum = d;
          v.t = k * 917.3f;
          v.mik = k / 3.0f; v.ses = 1.0f - k / 3.0f;
          v.pil = k == 0 ? -1 : k * 40;          // 120 de denenir
          v.wifi = k & 1; v.rssi = -40 - k * 15;
          v.saat = k == 0 ? -1 : 23; v.dakika = 59; v.gun = 31; v.ay = k == 3 ? 13 : 12; v.haftaGunu = 6;
          v.hava = y; v.altyazi = y; v.kullanici = y; v.etkinlik = y; v.bildirim = k == 2 ? y : "";
          v.vurguBas = k == 1 ? 5 : -1; v.etkinlikSn = k - 1; v.bootIpucu = k == 3; v.ota = k * 50;
          auto a = std::chrono::steady_clock::now();
          ekran::ciz(t.data(), v);
          double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
          if (ms > enUzunMs) enUzunMs = ms;
          kare++;
        }
    bool nobet = true;
    for (int i = 0; i < 64; i++) nobet &= t[ekran::GEN * ekran::YUK + i] == 0xBEEF;
    CHECK(nobet, "tampon disina yazildi");
    printf("   %d kare çizildi, en uzunu %.2f ms (masaüstünde)\n", kare, enUzunMs);
  }

  printf("== 4) içerik yerinde: saat, hava, not, altyazı\n");
  {
    std::vector<uint16_t> t(ekran::GEN * ekran::YUK);
    ekran::Veri v;
    v.durum = ekran::UYKU; v.saat = 3; v.dakika = 15; v.gun = 20; v.ay = 10; v.haftaGunu = 2; v.pil = 79;
    ekran::ciz(t.data(), v);
    int saatPx = farkli(t.data(), 30, 40, 130, 72);
    int altBos = farkli(t.data(), 4, 110, 100, 126);
    CHECK(saatPx > 150, "saat cizilmedi %d", saatPx);
    v.hava = "18° parçalı bulut";
    ekran::ciz(t.data(), v);
    int altHava = farkli(t.data(), 4, 110, 100, 126);
    CHECK(altHava > altBos + 40, "hava cizilmedi %d/%d", altHava, altBos);
    v.saat = -1;
    ekran::ciz(t.data(), v);
    CHECK(farkli(t.data(), 48, 77, 112, 91) == 0, "saat yokken tarih yazildi");
    v.durum = ekran::KONUSUYOR; v.altyazi = "Keşke. Ama biri uyuyacaksa o sensin. Ben zaten nöbetteyim.";
    v.vurguBas = -1;
    ekran::ciz(t.data(), v);
    std::vector<uint16_t> t2(t);
    v.vurguBas = (int)strlen("Keşke. Ama biri uyuyacaksa o sensin. Ben zaten ");
    ekran::ciz(t.data(), v);
    int degisen = 0;
    for (size_t i = 0; i < t.size(); i++) degisen += t[i] != t2[i];
    CHECK(degisen > 20 && degisen < 600, "vurgu yalnizca son parcayi boyamali: %d", degisen);
    v.durum = ekran::DINLIYOR; v.kullanici = ""; v.bildirim = "";
    ekran::ciz(t.data(), v);
    t2 = t;
    v.bildirim = "Mikrofonlardan ses gelmiyor.";
    ekran::ciz(t.data(), v);
    CHECK(farkli(t.data(), 0, 105, 160, 128) > 0 && memcmp(t.data(), t2.data(), t.size() * 2) != 0, "dinlerken not");
  }

  printf("\nSONUC: %s (%d hata)\n", fails ? "BASARISIZ" : "GECTI", fails);
  return fails ? 1 : 0;
}
