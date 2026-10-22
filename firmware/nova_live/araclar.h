// Nova'nın cihaz araçları: arka modele (GPT-Live delegation) "function" olarak verilir.
// Arka model bir aracı çağırınca link.cpp bunu aracCalistir'a verir; dönen yazı modele sonuç olarak gider.
//
// Araçlar: ses seviyesi, ekran parlaklığı, cihaz durumu, ayar değiştirme (araya girme, uyandırma
// hassasiyeti, ses tonu), hafıza (kaydet, sil), zamanlayıcı ve alarm (kur, listele, iptal).
// Donanıma AracOrtam üzerinden dokunur; bu sayede masaüstünde sınanır.
#pragma once
#include <stdint.h>
#include <stddef.h>

// session.start içinde delegation.responses.tools'a eklenen JSON dizisi
extern const char ARAC_TANIMLARI[];

struct AracOrtam {
  int  (*ses)() = nullptr;                    // 0-100
  void (*sesAyarla)(int yuzde) = nullptr;
  int  (*parlaklik)() = nullptr;
  void (*parlaklikAyarla)(int yuzde) = nullptr;
  // Ayarlar: değer yazısı ("on"/"off", "very_sensitive", ses adı). Sonucu msg'ye yazar; başarı true.
  bool (*ayar)(const char *ad, const char *deger, char *msg, size_t cap) = nullptr;
  void (*durum)(char *out, size_t cap) = nullptr;   // pil, WiFi, ayarlar
  int64_t (*simdi)() = nullptr;               // UTC epoch saniye; saat henüz alınmadıysa 0
  uint32_t (*ms)() = nullptr;                 // açılıştan beri ms
};

void aracBegin(const AracOrtam &o);
// Aracı çalıştırır; sonucu (modelin okuyacağı kısa Türkçe yazı) out'a yazar.
void aracCalistir(const char *ad, const char *argsJson, char *out, size_t cap);
// Ekranda "düşünüyor" altında görünen kısa etiket ("not alıyor" gibi); bilinmeyen araç için "".
const char *aracEtiketi(const char *ad);

// Yerel yedek: kullanıcının yazıya dökülmüş sözünde ses komutu var mı ("sesi kıs", "sesi biraz aç",
// "sesi yüzde 40 yap", "sesi sonuna kadar aç"). Varsa yeni seviyeyi (0-100), yoksa -1 döner.
int sesliSesKomutu(const char *metin, int simdiki);
// Yerel yedek sesi değiştirdi: kısa süre içinde gelen set_volume çağrısı ikinci kez değiştirmez.
void aracYerelSesYapildi(int yeniSeviye);
