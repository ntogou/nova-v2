// Masaüstü testi: hafıza (notlar, zamanlayıcılar, kayıt), cihaz araçları ve sesle ses komutu.
#include "hafiza.h"
#include "araclar.h"
#include "hava.h"
#include "notcikar.h"
#include "gundem.h"
#include "agistek.h"
#include "config.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  !! BASARISIZ (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string depoIcerik;
static int yazmalar = 0;
static bool depoOku(char *buf, size_t cap, size_t *len) {
  if (depoIcerik.empty() || depoIcerik.size() > cap) return false;
  memcpy(buf, depoIcerik.data(), depoIcerik.size());
  *len = depoIcerik.size();
  return true;
}
static bool depoYaz(const char *buf, size_t len) { depoIcerik.assign(buf, len); yazmalar++; return true; }

// Sahte cihaz
static int ses = 80, parlak = 80;
static int64_t simdiE = 0;
static uint32_t msNow = 100000;
static std::string sonAyar;
static int fSes() { return ses; }
static void fSesAyarla(int v) { ses = v; }
static int fParlak() { return parlak; }
static void fParlakAyarla(int v) { parlak = v; }
static bool fAyar(const char *ad, const char *d, char *msg, size_t cap) {
  sonAyar = std::string(ad) + "=" + d;
  snprintf(msg, cap, "ayar %s = %s", ad, d);
  return true;
}
static void fDurum(char *o, size_t c) { snprintf(o, c, "pil %%62"); }
static int64_t fSimdi() { return simdiE; }
static uint32_t fMs() { return msNow; }

static std::string arac(const char *ad, const char *args) {
  static char out[1024];
  out[0] = 0;
  aracCalistir(ad, args, out, sizeof(out));
  return out;
}
static bool has(const std::string &s, const char *k) { return s.find(k) != std::string::npos; }

int main() {
  hafiza::Depo d;
  d.oku = depoOku;
  d.yaz = depoYaz;
  CHECK(hafiza::begin(d, YEREL_SAAT_FARKI), "begin");
  AracOrtam o;
  o.ses = fSes; o.sesAyarla = fSesAyarla; o.parlaklik = fParlak; o.parlaklikAyarla = fParlakAyarla;
  o.ayar = fAyar; o.durum = fDurum; o.simdi = fSimdi; o.ms = fMs;
  aracBegin(o);
  // 18.10.2026 14:30 Türkiye = 11:30 UTC
  simdiE = hafiza::yerelEpoch(2026, 10, 18, 14, 30);
  {
    int y, m, dd, h, mi, wd;
    hafiza::yerelTakvim(simdiE, &y, &m, &dd, &h, &mi, &wd);
    CHECK(y == 2026 && m == 10 && dd == 18 && h == 14 && mi == 30 && wd == 0, "takvim %d-%d-%d %d:%d gun %d", y, m, dd, h, mi, wd);
    CHECK(simdiE == 1792323000LL, "epoch %lld", (long long)simdiE);     // 2026-10-18T11:30:00Z
  }

  printf("== 1) hafıza: ekle, aynısı tekrar eklenmez, prompt bloğu\n");
  std::string r = arac("remember", "{\"text\":\"Kullanıcının kedisinin adı Pamuk.\",\"source\":\"user_asked\"}");
  CHECK(has(r, "kaydedildi (#1)"), "%s", r.c_str());
  r = arac("remember", "{\"text\":\"kullanıcının kedisinin adı pamuk\",\"source\":\"own_initiative\"}");
  CHECK(has(r, "zaten kayıtlı"), "%s", r.c_str());
  r = arac("remember", "{\"text\":\"Çayı şekersiz içer.\",\"source\":\"own_initiative\"}");
  CHECK(has(r, "#2"), "%s", r.c_str());
  r = arac("remember", "{\"text\":\"Kız kardeşinin adı Ela.\",\"source\":\"user_confirmed\"}");
  CHECK(hafiza::notSayisi() == 3, "not sayisi %d", hafiza::notSayisi());
  char blok[2048];
  hafiza::promptaYaz(blok, sizeof(blok));
  CHECK(has(blok, "- (18.10) Kullanıcının kedisinin adı Pamuk.") && has(blok, "Çayı şekersiz"), "%s", blok);
  char liste[2048];
  hafiza::listele(liste, sizeof(liste));
  CHECK(has(liste, "#2 (18.10, Nova) Çayı şekersiz içer.") && has(liste, "onayladın"), "%s", liste);
  printf("%s", liste);

  printf("== 2) kayıt ve yeniden yükleme\n");
  CHECK(hafiza::degisti() && hafiza::kaydet() && !hafiza::degisti() && yazmalar == 1, "kaydet");
  CHECK(hafiza::kaydet() && yazmalar == 1, "degismeden yazildi");
  CHECK(hafiza::begin(d, YEREL_SAAT_FARKI) && hafiza::notSayisi() == 3, "yeniden yukleme %d", hafiza::notSayisi());
  r = arac("remember", "{\"text\":\"Bir not daha\",\"source\":\"user_asked\"}");
  CHECK(has(r, "#4"), "numara surmedi: %s", r.c_str());

  printf("== 3) unut: kelimeyle, numarayla, çok eşleşme, hepsi\n");
  r = arac("forget", "{\"query\":\"pamuk\"}");
  CHECK(has(r, "silindi") && has(r, "Pamuk") && hafiza::notSayisi() == 3, "%s", r.c_str());
  r = arac("forget", "{\"query\":\"yok böyle bir şey\"}");
  CHECK(has(r, "eşleşen not yok"), "%s", r.c_str());
  r = arac("forget", "{\"query\":\"#2\"}");
  CHECK(has(r, "Çayı") && hafiza::notSayisi() == 2, "%s", r.c_str());
  for (int i = 0; i < 5; i++) {
    char t[64];
    snprintf(t, sizeof(t), "{\"text\":\"Proje notu %d\",\"source\":\"own_initiative\"}", i);
    arac("remember", t);
  }
  r = arac("forget", "{\"query\":\"proje\"}");
  CHECK(has(r, "daha belirgin") && hafiza::notSayisi() == 7, "%s", r.c_str());
  r = arac("forget", "{\"query\":\"all\"}");
  CHECK(has(r, "bütün notlar silindi (7)") && hafiza::notSayisi() == 0, "%s", r.c_str());
  hafiza::promptaYaz(blok, sizeof(blok));
  CHECK(blok[0] == 0, "bos hafiza blok verdi");

  printf("== 4) dolu hafıza ve uzun not\n");
  for (int i = 0; i < HAFIZA_MAX_NOT; i++) {
    char t[64];
    snprintf(t, sizeof(t), "{\"text\":\"Not %d\",\"source\":\"user_asked\"}", i);
    arac("remember", t);
  }
  r = arac("remember", "{\"text\":\"Bir fazlası\",\"source\":\"user_asked\"}");
  CHECK(has(r, "hafıza dolu"), "%s", r.c_str());
  arac("forget", "{\"query\":\"all\"}");
  std::string uzun = "{\"text\":\"";
  for (int i = 0; i < 120; i++) uzun += "ş";
  uzun += "\",\"source\":\"user_asked\"}";
  r = arac("remember", uzun.c_str());
  hafiza::listele(liste, sizeof(liste));
  {
    // Kesilen not geçerli UTF-8 olmalı (iki baytlık harf ortadan bölünmemeli)
    const char *p = strstr(liste, "ş");
    size_t n = 0;
    while (p && p[n] && p[n] != '\n') n++;
    CHECK(p && n % 2 == 0 && n <= HAFIZA_METIN, "uzun not %zu bayt", n);
  }
  arac("forget", "{\"query\":\"all\"}");

  printf("== 5) zamanlayıcı ve alarm\n");
  r = arac("set_timer", "{\"minutes\":10,\"label\":\"çay\"}");
  CHECK(has(r, "Zamanlayıcı kuruldu (#1): çay, 14:40 (10 dk sonra)"), "%s", r.c_str());
  r = arac("set_alarm", "{\"time\":\"08:00\",\"label\":\"uyan\"}");
  CHECK(has(r, "Alarm kuruldu (#2): uyan, yarın 08:00"), "%s", r.c_str());
  r = arac("set_alarm", "{\"time\":\"15:00\",\"label\":\"ilaç\"}");
  CHECK(has(r, "ilaç, 15:00 (30 dk sonra)"), "%s", r.c_str());
  r = arac("set_alarm", "{\"time\":\"09:15\",\"date\":\"2026-10-20\",\"label\":\"toplantı\"}");
  CHECK(has(r, "20.10 09:15"), "%s", r.c_str());
  r = arac("set_alarm", "{\"time\":\"25:00\",\"label\":\"x\"}");
  CHECK(has(r, "Hata"), "%s", r.c_str());
  r = arac("list_timers", "{}");
  CHECK(has(r, "#1 zamanlayıcı: çay (14:40, 10 dk sonra); #3 alarm: ilaç (15:00, 30 dk sonra); #2 alarm: uyan (yarın 08:00); #4 alarm: toplantı (20.10 09:15)"), "%s", r.c_str());
  r = arac("cancel_timer", "{\"query\":\"ilaç\"}");
  CHECK(has(r, "iptal edildi: ilaç") && hafiza::zamanlayiciSayisi() == 3, "%s", r.c_str());
  hafiza::Calan c;
  CHECK(!hafiza::calanVar(simdiE + 599, &c), "erken caldi");
  CHECK(hafiza::calanVar(simdiE + 600, &c) && c.id == 1 && !strcmp(c.etiket, "çay") && !c.kacirildi, "calmadi");
  CHECK(hafiza::kaydet() && hafiza::begin(d, YEREL_SAAT_FARKI) && hafiza::zamanlayiciSayisi() == 2, "zamanlayici kaydi");
  // Cihaz kapalıyken geçen alarm: kaçırıldı sayılır
  CHECK(hafiza::calanVar(simdiE + 86400 * 2, &c) && c.kacirildi, "kacirilan");
  simdiE = 0;
  r = arac("set_timer", "{\"minutes\":5,\"label\":\"x\"}");
  CHECK(has(r, "saati henüz"), "%s", r.c_str());
  simdiE = hafiza::yerelEpoch(2026, 10, 18, 14, 30);

  printf("== 6) ses ve parlaklık araçları\n");
  r = arac("set_volume", "{\"level\":40}");
  CHECK(ses == 40 && has(r, "%40"), "%s", r.c_str());
  r = arac("set_volume", "{\"change\":\"up\"}");
  CHECK(ses == 50, "ses %d", ses);
  r = arac("set_volume", "{\"change\":\"down\",\"step\":30}");
  CHECK(ses == 20, "ses %d", ses);
  r = arac("set_brightness", "{\"level\":0}");
  CHECK(parlak == 5, "parlaklik %d", parlak);
  r = arac("change_setting", "{\"setting\":\"wake_sensitivity\",\"value\":\"very_sensitive\"}");
  CHECK(sonAyar == "wake_sensitivity=very_sensitive", "%s", sonAyar.c_str());
  r = arac("bilinmeyen_arac", "{}");
  CHECK(has(r, "Hata"), "%s", r.c_str());

  printf("== 7) sesle ses komutu (yerel yedek) ve model aracıyla çift değişiklik yok\n");
  struct { const char *s; int bas; int bek; } K[] = {
      {"Sesi kıs", 80, 60}, {"sesini biraz açar mısın", 80, 90}, {"sesi yüzde 40 yap", 80, 40},
      {"Sesi sonuna kadar aç", 50, 100}, {"Ses seviyesini 30 yap", 80, 30}, {"sesi çok yüksek kısar mısın", 80, 50},
      {"Sesin çok güzel", 80, -1}, {"Ses açık mı", 80, -1}, {"bugün hava nasıl", 80, -1}, {"sesi azalt", 10, 0},
      {"Nova sesini yükselt", 60, 80}, {"sessiz ol", 80, -1}, {"sesli kitap aç", 80, -1},
  };
  for (auto &k : K) {
    int v = sesliSesKomutu(k.s, k.bas);
    CHECK(v == k.bek, "'%s' (%d) -> %d, beklenen %d", k.s, k.bas, v, k.bek);
  }
  ses = 80;
  int v = sesliSesKomutu("sesi kıs", ses);
  fSesAyarla(v);
  aracYerelSesYapildi(v);
  msNow += 3000;
  r = arac("set_volume", "{\"change\":\"down\"}");
  CHECK(ses == 60 && has(r, "zaten"), "cift degisti: ses %d, %s", ses, r.c_str());
  msNow += 30000;
  r = arac("set_volume", "{\"change\":\"down\"}");
  CHECK(ses == 50, "sonraki istek uygulanmadi: %d", ses);

  printf("== 8) araç tanımları geçerli JSON\n");
  JsonDocument td;
  CHECK(!deserializeJson(td, ARAC_TANIMLARI) && td.as<JsonArray>().size() == 10, "arac tanimlari");
  for (JsonVariant t : td.as<JsonArray>()) CHECK(t["type"] == "function" && t["name"].is<const char *>() && t["parameters"]["type"] == "object", "arac bicimi");

  printf("== 9) hava durumu cevabı Türkçe özete çevrilir, eskiyince verilmez\n");
  {
    const char *J = "{\"latitude\":38.42,\"current\":{\"time\":\"2026-10-18T14:30\",\"interval\":900,\"temperature_2m\":19.4,"
                    "\"apparent_temperature\":18.2,\"relative_humidity_2m\":61,\"weather_code\":2,\"wind_speed_10m\":12.3},"
                    "\"daily\":{\"time\":[\"2026-10-18\",\"2026-10-19\",\"2026-10-20\"],\"weather_code\":[2,63,0],"
                    "\"temperature_2m_max\":[22.1,20.4,23.0],\"temperature_2m_min\":[14.2,15.0,13.6],\"precipitation_probability_max\":[10,70,0]}}";
    char oz[700];
    CHECK(havaCoz(J, strlen(J), "İzmir", oz, sizeof(oz)), "hava cozulmedi");
    printf("%s\n", oz);
    CHECK(has(oz, "İzmir, saat 14:30") && has(oz, "şu an 19°C (hissedilen 18), parçalı bulutlu, nem %61, rüzgâr 12 km/sa.") &&
          has(oz, "Bugün: 14–22°C, parçalı bulutlu, yağış ihtimali %10.") && has(oz, "Yarın: 15–20°C, yağmurlu, yağış ihtimali %70.") &&
          has(oz, "Salı: 14–23°C, açık"), "hava ozeti");
    CHECK(!havaCoz("{\"hata\":1}", 11, "x", oz, sizeof(oz)), "bozuk cevap kabul edildi");
    havaSakla("ozet", 1000);
    char hy[100];
    CHECK(havaYazi(hy, sizeof(hy), 1000 + 60000) && !strcmp(hy, "ozet"), "hava yazisi");
    CHECK(!havaYazi(hy, sizeof(hy), 1000 + HAVA_ESKI_MS + 1), "eski hava verildi");
    // ekranın kısa hali
    CHECK(!havaKisa(hy, sizeof(hy), 1000 + 60000), "ozetten kisa hal cikti: %s", hy);   // "ozet": şu an yok
    havaSakla(oz, 5000);
    CHECK(havaKisa(hy, sizeof(hy), 6000) && !strcmp(hy, UI_TR("19° parçalı bulut", "19° partly cloudy")), "kisa: %s", hy);
    CHECK(!havaKisa(hy, sizeof(hy), 5000 + HAVA_ESKI_MS + 1), "eski kisa verildi");
    havaSakla("Hava durumu (İzmir, saat 09:00 itibarıyla): şu an -3°C (hissedilen -7), gök gürültülü fırtınalı, nem %90.", 5000);
    CHECK(havaKisa(hy, sizeof(hy), 6000) && !strcmp(hy, UI_TR("-3° fırtına", "-3° storm")), "eksi: %s", hy);
    havaSakla("Hava durumu (İzmir, saat 09:00 itibarıyla): şu an 7°C (hissedilen 5), değişken, nem %90.", 5000);
    CHECK(havaKisa(hy, sizeof(hy), 6000) && !strcmp(hy, "7°"), "degisken: %s", hy);
    havaSakla("Hava durumu (İzmir, saat 09:00 itibarıyla): şu an 10°C (hissedilen 9), kapalı, nem %90.", 5000);
    CHECK(havaKisa(hy, sizeof(hy), 6000) && !strcmp(hy, UI_TR("10° kapalı", "10° overcast")), "kapali: %s", hy);
    havaSakla("Hava durumu (İzmir, saat 09:00 itibarıyla): şu an 7°C (hissedilen 5), değişken, nem %90.", 5000);
    char kucuk[3];   // "7°" 3 bayt + sonlandırıcı sığmaz
    CHECK(!havaKisa(kucuk, sizeof(kucuk), 6000) && !kucuk[0], "kucuk tampon");
  }
  printf("== 9b) araç etiketleri (ekranda düşünürken)\n");
  CHECK(!strcmp(aracEtiketi("remember"), UI_TR("not alıyor", "taking a note")) &&
        !strcmp(aracEtiketi("set_alarm"), UI_TR("alarm kuruyor", "setting an alarm")) &&
        !strcmp(aracEtiketi("bilinmeyen"), "") && !strcmp(aracEtiketi(nullptr), ""), "etiketler");

  printf("== 10) oturum sonu not çıkarma: istek gövdesi geçerli JSON, cevap hafızaya uygulanır\n");
  {
    arac("forget", "{\"query\":\"all\"}");
    arac("remember", "{\"text\":\"Kullanıcının köpeğinin adı Karabaş.\",\"source\":\"user_asked\"}");
    // Kısa konuşma sıraya alınmaz
    notKonusmaEkle(true, "Görüşürüz");
    notKonusmaEkle(false, "Görüşürüz.");
    CHECK(!notOturumBitti() && !notBekliyor(), "kisa konusma siraya alindi");
    notKonusmaEkle(true, "İki tane su kaplumbağam var, adları Badem ve Fındık. \"Onları\" da yaz.");
    notKonusmaEkle(false, "Not aldım.");
    CHECK(notOturumBitti() && notBekliyor(), "konusma siraya alinmadi");
    char notlar[2048];
    hafiza::numaraliListe(notlar, sizeof(notlar));
    CHECK(has(notlar, "#") && has(notlar, "Karabaş"), "%s", notlar);
    static char g[32768];
    size_t gl = notIstekGovdesi("gpt-6-luna", notlar, "Kullanıcı: Badem ve \"Fındık\"\nNova: Not aldım.\n", g, sizeof(g));
    JsonDocument gd;
    CHECK(gl > 0 && !deserializeJson(gd, g, gl), "govde JSON degil: %.200s", g);
    CHECK(gd["model"] == "gpt-6-luna" && gd["store"] == false && gd["text"]["format"]["type"] == "json_schema" &&
          gd["text"]["format"]["strict"] == true && has(gd["input"] | "", "Karabaş") && has(gd["input"] | "", "\"Fındık\"") &&
          gd["text"]["format"]["schema"]["required"][1] == "sil", "govde alanlari");
    // Responses API cevabı (yapılandırılmış çıktı output_text içinde, JSON yazısı olarak)
    const char *CEVAP = "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"summary\":[]},"
                        "{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":"
                        "\"{\\\"ekle\\\":[{\\\"metin\\\":\\\"Kullanıcının Badem ve Fındık adında iki su kaplumbağası var.\\\",\\\"kaynak\\\":\\\"user_asked\\\"},"
                        "{\\\"metin\\\":\\\"Kullanıcının köpeğinin adı Karabaş.\\\",\\\"kaynak\\\":\\\"own_initiative\\\"}],\\\"sil\\\":[]}\"}]}]}";
    char rapor[600];
    int n = notCevabiUygula(CEVAP, strlen(CEVAP), 20261018, rapor, sizeof(rapor));
    CHECK(n == 1 && hafiza::notSayisi() == 2 && has(rapor, "Badem ve Fındık"), "uygulama: %d, %s", n, rapor);
    printf("   %s\n", rapor);
    hafiza::listele(liste, sizeof(liste));
    CHECK(has(liste, "(18.10, senin isteğin) Kullanıcının Badem ve Fındık"), "%s", liste);
    // Silme
    const char *SIL = "{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"ekle\\\":[],\\\"sil\\\":[\\\"#1\\\"]}\"}]}]}";
    n = notCevabiUygula(SIL, strlen(SIL), 20261018, rapor, sizeof(rapor));
    CHECK(n == 1 && hafiza::notSayisi() == 1 && has(rapor, "Karabaş"), "silme: %d, %s", n, rapor);
    const char *BOS = "{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"{\\\"ekle\\\":[],\\\"sil\\\":[]}\"}]}]}";
    n = notCevabiUygula(BOS, strlen(BOS), 20261018, rapor, sizeof(rapor));
    CHECK(n == 0 && has(rapor, "yeni not yok"), "bos: %s", rapor);
    const char *HATA = "{\"error\":{\"message\":\"model yok\"},\"status\":\"failed\"}";
    CHECK(notCevabiUygula(HATA, strlen(HATA), 0, rapor, sizeof(rapor)) < 0 && has(rapor, "model yok"), "hata: %s", rapor);
    arac("forget", "{\"query\":\"all\"}");
  }

  printf("== 11) HTTP cevabı ayırma ve parçalı (chunked) gövde\n");
  {
    char h1[] = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"a\":1}";
    char *g = nullptr; size_t gl = 0;
    CHECK(httpAyir(h1, strlen(h1), &g, &gl) == 200 && gl == 7 && !strncmp(g, "{\"a\":1}", 7), "duz cevap");
    char h2[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\n{\"a\"\r\n3\r\n:12\r\n1\r\n}\r\n0\r\n\r\n";
    CHECK(httpAyir(h2, strlen(h2), &g, &gl) == 200 && gl == 8 && !strcmp(g, "{\"a\":12}"), "parcali: %zu '%s'", gl, g);
    char h3[] = "HTTP/1.1 401 Unauthorized\r\n\r\n{}";
    CHECK(httpAyir(h3, strlen(h3), &g, &gl) == 401, "401");
    char h4[] = "cop";
    CHECK(httpAyir(h4, strlen(h4), &g, &gl) == -1, "cop");
  }

  printf("== 12) eski yerden taşınan kayıt hemen yeni yere yazılır (2.7.0 bölüm tablosu)\n");
  {
    arac("remember", "{\"text\":\"Salyangozlarin adi Test.\",\"source\":\"user_asked\"}");
    CHECK(hafiza::kaydet(), "kaydet");
    std::string eski = depoIcerik;
    hafiza::Depo t = d;
    t.tasindi = []() { return true; };
    CHECK(hafiza::begin(t, YEREL_SAAT_FARKI) && hafiza::notSayisi() == 1, "tasima yukleme %d", hafiza::notSayisi());
    CHECK(hafiza::degisti(), "tasinan kayit kirli sayilmadi (yeni yere yazilmaz)");
    int y0 = yazmalar;
    CHECK(hafiza::kaydet() && yazmalar == y0 + 1 && depoIcerik == eski, "tasinan kayit ayni yazilmadi");
    CHECK(hafiza::begin(d, YEREL_SAAT_FARKI) && !hafiza::degisti(), "normal yuklemede kirli");
    arac("forget", "{\"query\":\"all\"}");
    hafiza::kaydet();
  }

  printf("== 13) gündem: istek gövdesi, cevap çözme, bağlantı temizleme, saatler, talimat bloğu\n");
  {
    static char g[4096];
    size_t gl = gundemIstekGovdesi("gpt-6-luna", "18.10.2026 Pazar, saat 13:05", g, sizeof(g));
    JsonDocument j;
    CHECK(gl > 0 && !deserializeJson(j, g, gl), "govde JSON degil");
    CHECK(j["background"] == true && j["tools"][0]["type"] == "web_search" && j["max_tool_calls"] == GUNDEM_MAX_ARAMA &&
          j["tools"][0]["user_location"]["country"] == "TR" && has(j["input"] | "", "18.10.2026") &&
          has(j["instructions"] | "", "Teknoloji") && !j["store"].is<bool>(), "govde alanlari: %s", g);
    CHECK(gundemIstekGovdesi("m", "x", g, 200) == 0, "sigmayan govde");

    // temizleme
    char t1[] = "**Genel:**\n- Fed faizi indirdi ([reuters.com](https://www.reuters.com/x?utm_source=openai)).\n"
                "- [OpenAI](https://openai.com) yeni model duyurdu (apnews.com).\n\n"
                "## Teknoloji:\n-  Çip satışları %3.5 arttı  ; bkz https://example.com/a  \n- \n";
    gundemTemizle(t1);
    CHECK(!strcmp(t1, "Genel:\n- Fed faizi indirdi.\n- OpenAI yeni model duyurdu.\nTeknoloji:\n- Çip satışları %3.5 arttı; bkz"),
          "temizle: [%s]", t1);
    char t2[] = "GPT-5.5 (3.5 kat hızlı) geldi (bbc.co.uk/news).";
    gundemTemizle(t2);
    CHECK(!strcmp(t2, "GPT-5.5 (3.5 kat hızlı) geldi."), "temizle2: [%s]", t2);

    // oluşturma cevabı: kuyrukta
    GundemCevap c;
    char oz[GUNDEM_CAP + 1], hata[240] = "";
    const char *K = "{\"id\":\"resp_abc123\",\"object\":\"response\",\"status\":\"queued\",\"output\":[]}";
    CHECK(gundemCevapCoz(K, strlen(K), &c, oz, sizeof(oz), hata, sizeof(hata)) && !strcmp(c.id, "resp_abc123") &&
          !strcmp(c.durum, "queued") && !c.metinVar, "kuyruk: %s %s", c.id, c.durum);
    // yoklama cevabı: bitti, arama + mesaj
    const char *B = "{\"id\":\"resp_abc123\",\"status\":\"completed\",\"output\":[{\"type\":\"reasoning\",\"summary\":[]},"
                    "{\"type\":\"web_search_call\",\"status\":\"completed\",\"action\":{\"type\":\"search\",\"query\":\"haber\"}},"
                    "{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"Genel:\\n- Deprem oldu "
                    "([aa.com.tr](https://aa.com.tr/x)).\\nTeknoloji:\\n- Yeni çip.\",\"annotations\":[{\"type\":\"url_citation\","
                    "\"url\":\"https://aa.com.tr/x\",\"title\":\"AA\",\"start_index\":20,\"end_index\":50}]}]}]}";
    CHECK(gundemCevapCoz(B, strlen(B), &c, oz, sizeof(oz), hata, sizeof(hata)) && c.metinVar &&
          !strcmp(oz, "Genel:\n- Deprem oldu.\nTeknoloji:\n- Yeni çip."), "bitti: [%s]", oz);
    const char *F = "{\"id\":\"resp_x\",\"status\":\"failed\",\"error\":{\"message\":\"kota\"},\"output\":[]}";
    hata[0] = 0;
    CHECK(gundemCevapCoz(F, strlen(F), &c, oz, sizeof(oz), hata, sizeof(hata)) && !c.metinVar && has(hata, "failed") &&
          has(hata, "kota"), "basarisiz: %s", hata);
    const char *E = "{\"error\":{\"message\":\"Unsupported parameter: max_tool_calls\"}}";
    CHECK(!gundemCevapCoz(E, strlen(E), &c, oz, sizeof(oz), hata, sizeof(hata)) && has(hata, "max_tool_calls"), "hata: %s", hata);

    // saatler (8, 13, 19 yerel)
    int64_t s1205 = hafiza::yerelEpoch(2026, 10, 18, 12, 5), s1305 = hafiza::yerelEpoch(2026, 10, 18, 13, 5);
    int64_t s0810 = hafiza::yerelEpoch(2026, 10, 18, 8, 10), s0700 = hafiza::yerelEpoch(2026, 10, 18, 7, 0);
    int64_t dun1930 = hafiza::yerelEpoch(2026, 10, 17, 19, 30), dun1830 = hafiza::yerelEpoch(2026, 10, 17, 18, 30);
    CHECK(gundemZamani(0, s1205, YEREL_SAAT_FARKI), "hic cekilmedi");
    CHECK(!gundemZamani(s0810, s1205, YEREL_SAAT_FARKI), "08:10'da cekildi, 12:05'te tekrar");
    CHECK(gundemZamani(s0810, s1305, YEREL_SAAT_FARKI), "13:05'te yenilenmedi");
    CHECK(!gundemZamani(dun1930, s0700, YEREL_SAAT_FARKI), "dun 19:30'dan sonra sabah 7'de cekildi");
    CHECK(gundemZamani(dun1830, s0700, YEREL_SAAT_FARKI), "dun 18:30'dan sonra 19 kacirildi");
    CHECK(!gundemZamani(s1305, 0, YEREL_SAAT_FARKI), "saat bilinmiyor");

    // saklama ve talimat bloğu
    char yb[GUNDEM_CAP + 96];
    CHECK(gundemYazi(yb, sizeof(yb), s1305, YEREL_SAAT_FARKI) == 0, "bos gundem yazildi");
    gundemSakla("Genel:\n- Deprem oldu.", s0810);
    CHECK(gundemYazi(yb, sizeof(yb), s1205, YEREL_SAAT_FARKI) && has(yb, "bugün saat 08:10 itibarıyla") && has(yb, "Deprem"), "blok: %s", yb);
    CHECK(gundemYazi(yb, sizeof(yb), hafiza::yerelEpoch(2026, 10, 19, 7, 0), YEREL_SAAT_FARKI) == 0, "14 saatten eski verildi");
    gundemSakla("Genel:\n- X.", dun1930);
    CHECK(gundemYazi(yb, sizeof(yb), s0700, YEREL_SAAT_FARKI) && has(yb, "dün saat 19:30"), "dun: %s", yb);
    std::string uzun(GUNDEM_CAP * 2, 'a');
    uzun[GUNDEM_CAP - 1] = (char)0xC5; uzun[GUNDEM_CAP] = (char)0x9F;   // "ş" sınırda bölünmesin
    gundemSakla(uzun.c_str(), s0810);
    CHECK(strlen(gundemMetni()) == GUNDEM_CAP - 1, "kesme %zu", strlen(gundemMetni()));
    CHECK(gundemVakti() == s0810, "vakit");
    gundemSakla("", s0810);
    CHECK(gundemVakti() == 0, "bos metin vakit sifirlamadi");
  }

  printf("\nSONUC: %s (%d hata)\n", fails ? "BASARISIZ" : "GECTI", fails);
  return fails ? 1 : 0;
}
