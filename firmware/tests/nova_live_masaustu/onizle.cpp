// Masaüstü önizleme: ekranın her durumunu 160x128 ham RGB565 dosyalarına yazar
#include "ekran.h"
#include <cstdio>
#include <cstring>
#include "config.h"
static uint16_t buf[ekran::GEN * ekran::YUK];
static void kaydet(const char *ad, const ekran::Veri &v) {
  ekran::ciz(buf, v);
  FILE *f = fopen(ad, "wb");
  fwrite(buf, 2, ekran::GEN * ekran::YUK, f);
  fclose(f);
}
int main() {
  ekran::Veri v;
  v.pil = 79; v.wifi = true; v.rssi = -52; v.saat = 3; v.dakika = 15; v.gun = 19; v.ay = 10; v.haftaGunu = 1;
  v.hava = UI_TR("10° kapalı", "10° overcast"); v.t = 1.3f;
  // kartta ui.cpp hava yazısını hava.cpp'nin havaKisa()'sından alır: "18° parçalı bulut" gibi
  v.durum = ekran::UYKU; kaydet("1_uyku.raw", v);
  v.bildirim = "Mikrofonlardan ses gelmiyor."; kaydet("1b_uyku_bildirim.raw", v); v.bildirim = "";
  v.durum = ekran::DINLIYOR; v.mik = 0.62f; kaydet("2_dinliyor.raw", v);
  v.kullanici = "Pazartesi sabahları sekiz çeyrekte dersim olduğunu hafızana yazar mısın"; kaydet("2b_dinliyor_soz.raw", v);
  v.kullanici = "";
  v.bildirim = "Mikrofonlardan ses gelmiyor."; kaydet("2c_dinliyor_bildirim.raw", v); v.bildirim = "";
  v.durum = ekran::DUSUNUYOR; v.etkinlik = UI_TR("araştırıyor…", "searching…"); v.etkinlikSn = 4; kaydet("3_dusunuyor.raw", v);
  v.etkinlik = UI_TR("not alıyor", "taking a note"); v.etkinlikSn = -1; kaydet("3b_dusunuyor_arac.raw", v);
  v.durum = ekran::KONUSUYOR; v.ses = 0.75f; v.kullanici = "Gel yerime de uyu o zaman";
  const char *a = "Keşke. Ama biri uyuyacaksa o sensin. Ben zaten nöbetteyim.";
  v.altyazi = a; v.vurguBas = (int)(strstr(a, "nöbetteyim") - a); kaydet("4_konusuyor.raw", v);
  const char *b = "Saat 3'ü 15 geçiyor. Yani uykuya şans vermek için hâlâ bir pencere var. İstersen sabah için bir alarm kurayım, ama ertelemeye basarsan hiç karışmam. Görüşürüz.";
  v.altyazi = b; v.vurguBas = (int)(strstr(b, "Görüşürüz") - b); kaydet("4b_konusuyor_uzun.raw", v);
  v.durum = ekran::ACILIS; kaydet("5_acilis.raw", v);
  v.bildirim = "Güvenli kip: art arda çöktü. Yeni yazılım yükle ya da R.  nova.local  192.168.1.23"; kaydet("5c_bakim.raw", v); v.bildirim = "";
  v.durum = ekran::WIFI; kaydet("5b_wifi.raw", v);
  v.durum = ekran::GUNCELLEME; v.ota = 43; kaydet("6_ota.raw", v);
  v.durum = ekran::PIL_BITTI; kaydet("7_pil.raw", v);
  return 0;
}
