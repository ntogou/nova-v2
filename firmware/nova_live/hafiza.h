// Nova'nın kalıcı hafızası: kullanıcı hakkında notlar ve kurulu zamanlayıcı / alarmlar.
//
// Çalışma kopyası PSRAM'de durur; araçlar (araclar.cpp) onu değiştirir. Değişiklik flaşa ancak cihaz
// boştayken yazılır (kaydet): flaşa yazarken önbellek kapanır, oturum sırasında ses takılmasın.
// Kartta kayıt, Arduino bölümlemesinin kullanılmayan "ffat" bölümüne dosya sistemi olmadan, iki
// yuvaya sırayla yazılır (yazarken güç giderse öbür yuvadaki önceki kayıt kalır).
//
// Notların üç kaynağı var (kullanıcının 18 Ekim kararı: üçü de olsun):
//   'k' kullanıcı "hatırla" dedi, 'o' Nova sordu ve kullanıcı onayladı, 'n' Nova kendisi kaydetti.
// Donanıma bağlı tek kısım flasDepo(); geri kalanı masaüstünde sınanır.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace hafiza {

enum : char { KAYNAK_KULLANICI = 'k', KAYNAK_ONAYLI = 'o', KAYNAK_NOVA = 'n' };
enum : char { ZAMANLAYICI = 't', ALARM = 'a' };

// Kalıcı saklama. oku: son geçerli kaydı buf'a koyar (yoksa false). yaz: kaydı saklar.
struct Depo {
  bool (*oku)(char *buf, size_t cap, size_t *len) = nullptr;
  bool (*yaz)(const char *buf, size_t len) = nullptr;
  bool (*tasindi)() = nullptr;   // oku() kaydı eski bir yerden getirdiyse true: begin() onu hemen yeni yere yazdırır
};
#ifndef NOVA_HOST_TEST
Depo flasDepo();                 // kartta: ffat bölümü, iki yuva
size_t flasDurum(char *out, size_t cap);   // açılış satırı için: depo boyutu, adresi, taşındıysa
#endif

// Kaydı yükler. tzSaniye: yerel saatin UTC'den farkı (Türkiye +10800).
bool begin(const Depo &d, int32_t tzSaniye);

// ---- notlar
// id döner; -1: eklenmedi (sebebi msg'de). ymd: YYYYMMDD (bilinmiyorsa 0).
int  ekle(const char *metin, char kaynak, int32_t ymd, char *msg, size_t cap);
// sorgu: "#3" (numara), "hepsi" ya da notun içinde geçen bir parça. Silinen sayısını döner (msg: ne silindi).
int  unut(const char *sorgu, char *msg, size_t cap);
int  notSayisi();
size_t promptaYaz(char *out, size_t cap);    // modele verilecek blok; not yoksa 0
size_t listele(char *out, size_t cap);       // kayıt sayfası için (numara, kaynak, tarih)
size_t numaraliListe(char *out, size_t cap); // "#3 metin" satırları (oturum sonu not çıkarmaya verilir)
// Sayfa için: her notu sırayla verir (kilit altında; f içinden hafıza işlevi çağrılmaz)
void notlariGez(void (*f)(void *ctx, int id, int32_t ymd, char kaynak, const char *metin), void *ctx);

// ---- zamanlayıcı ve alarm (zamanlar UTC epoch saniyesi)
int  kur(int64_t vakit, char tur, const char *etiket, int64_t simdi, char *msg, size_t cap);   // id; -1: kurulamadı
int  iptal(const char *sorgu, char *msg, size_t cap);                                           // "#2", "hepsi" ya da etiket parçası
size_t zamanlayicilar(char *out, size_t cap, int64_t simdi);                                    // liste; yoksa 0
int  zamanlayiciSayisi();
// Sayfa için: zamanlayıcı ve alarmları vakit sırasıyla verir (kilit altında)
void zamanlariGez(void (*f)(void *ctx, int id, char tur, const char *etiket, int64_t vakit), void *ctx);
struct Calan { int id; char tur; char etiket[64]; int64_t vakit; bool kacirildi; };
// Vakti gelmiş ilk kaydı çıkarır. Cihaz kapalıyken 30 dakikadan fazla geçmişse kacirildi = true.
bool calanVar(int64_t simdi, Calan *c);
// Yerel saat yazısı: "14:35", "yarın 08:00", "20.10 09:15"
void saatYaz(int64_t vakit, int64_t simdi, char *out, size_t cap);
// Yerel takvim tarihini (YYYY, AA, GG, SS, DD) UTC epoch'a çevirir
int64_t yerelEpoch(int yil, int ay, int gun, int saat, int dakika);
// UTC epoch'tan yerel takvim
void yerelTakvim(int64_t vakit, int *yil, int *ay, int *gun, int *saat, int *dakika, int *haftaGunu);

// ---- kayıt
bool degisti();
bool kaydet();                   // değiştiyse depoya yazar
// Karşılaştırma için sadeleştirme: küçük harf, Türkçe harfler yalın (ı İ -> i, ş -> s ...), harf/rakam dışı boşluk
void katla(const char *in, char *out, size_t cap);

}  // namespace hafiza
