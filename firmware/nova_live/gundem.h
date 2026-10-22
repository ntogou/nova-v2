// Gündem özeti: günün en çarpıcı genel olayları ve gerçekten önemli teknoloji gelişmeleri.
//
// Kullanıcının isteği (18 Ekim): uygulamadaki gibi güncel olsun; Nova haberleri aramaya gitmeden bilsin.
// Cihaz boştayken GUNDEM_SAATLER'deki her saatten sonra bir kez arka modele (Responses API, web araması)
// sorulur. İstek arka planda çalışır (background): kart isteği bırakır, sonucu aralıklarla yoklar; böylece
// uyandırma motoru 20 sn değil, her yoklamada 1-2 sn kalkar. Özet oturum açılınca talimata eklenir; ayrıntı
// ya da daha yeni bilgi istenirse Nova yine web'de arar. Son özet NVS'de saklanır (yeniden başlayınca
// boşuna yeniden çekilmez).
#pragma once
#include <stdint.h>
#include <stddef.h>

// ---- masaüstünde sınanan kısım
// tarih: "18.10.2026 Pazar, saat 13:05" gibi
size_t gundemIstekGovdesi(const char *model, const char *tarih, char *out, size_t cap);

struct GundemCevap {
  char durum[24];      // "queued", "in_progress", "completed", "failed", "incomplete", "cancelled"
  char id[96];
  bool metinVar;
};
// Responses API cevabını çözer (oluşturma ya da yoklama). Metin varsa temizlenip ozet'e yazılır.
// false: cevap JSON değil ya da kimlik/durum yok (sebep hata'da).
bool gundemCevapCoz(const char *json, size_t len, GundemCevap *c, char *ozet, size_t cap, char *hata, size_t hcap);
// Markdown bağlantılarını, kaynak site adlarını, çıplak adresleri ve ** işaretlerini siler. Yeni uzunluk.
size_t gundemTemizle(char *s);
// sonCekis'ten (UTC epoch, 0 = hiç) sonra bir gündem saati geçtiyse true. tz: yerel saat farkı (sn).
bool gundemZamani(int64_t sonCekis, int64_t simdi, int32_t tz);

void gundemSakla(const char *ozet, int64_t vakit);
// Talimata eklenecek blok; yoksa ya da GUNDEM_ESKI_SAAT'ten eskiyse 0.
size_t gundemYazi(char *out, size_t cap, int64_t simdi, int32_t tz);
int64_t gundemVakti();          // son başarılı özetin vakti (UTC epoch); 0 = yok
const char *gundemMetni();      // saklanan özet (NVS'ye yazmak için); yoksa ""

#ifndef NOVA_HOST_TEST
// İsteği arka planda başlatır. 1: başladı, 0: ağ hatası (sonra yeniden), -1: sunucu reddetti (bu saat geçilir).
int gundemBaslat(int64_t simdi, const char *tarih, char *hata, size_t hcap);
// Bekleyen isteği yoklar. 1: özet geldi ve saklandı, 0: sürüyor ya da ağ hatası, -1: başarısız ya da süre doldu.
int gundemYokla(int64_t simdi, char *hata, size_t hcap);
bool gundemBekliyor();
#endif
