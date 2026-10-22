#include "araclar.h"
#include "hafiza.h"
#include "config.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// Açıklamalar arka model için: kısa, ne zaman çağrılacağı ve argümanların biçimi.
const char ARAC_TANIMLARI[] = R"JSON([
{"type":"function","strict":false,"name":"set_volume","description":"Cihazın hoparlör ses seviyesini değiştirir. Ya level (0-100) ya da change (up/down) ver. 'Biraz' için step 10, normalde step verme.","parameters":{"type":"object","properties":{"level":{"type":"integer","minimum":0,"maximum":100},"change":{"type":"string","enum":["up","down"]},"step":{"type":"integer","minimum":5,"maximum":50}},"additionalProperties":false}},
{"type":"function","strict":false,"name":"set_brightness","description":"Cihazın ekran parlaklığını değiştirir. Ya level (0-100) ya da change (up/down) ver.","parameters":{"type":"object","properties":{"level":{"type":"integer","minimum":0,"maximum":100},"change":{"type":"string","enum":["up","down"]}},"additionalProperties":false}},
{"type":"function","strict":false,"name":"device_status","description":"Cihazın durumu: pil yüzdesi, WiFi, ses seviyesi, parlaklık, uyandırma hassasiyeti, araya girme, ses tonu, kurulu zamanlayıcılar, not sayısı.","parameters":{"type":"object","properties":{},"additionalProperties":false}},
{"type":"function","strict":false,"name":"change_setting","description":"Cihaz ayarını değiştirir. barge_in: on/off (Nova konuşurken kullanıcının araya girebilmesi). wake_sensitivity: off/normal/sensitive/very_sensitive ('Hey Nova' uyandırma sözü). voice: konuşma sesinin adı (ör. marin, cedar, gleam); bir sonraki oturumda geçerli olur.","parameters":{"type":"object","properties":{"setting":{"type":"string","enum":["barge_in","wake_sensitivity","voice"]},"value":{"type":"string"}},"required":["setting","value"],"additionalProperties":false}},
{"type":"function","strict":false,"name":"remember","description":"Kullanıcı hakkında kalıcı bir notu cihazın hafızasına kaydeder; not her konuşmanın başında sesli asistana verilir. text: üçüncü şahısla tek kısa cümle (ör. 'Kullanıcının kedisinin adı Pamuk.'). source: user_asked (kullanıcı hatırla dedi), user_confirmed (asistan sordu, kullanıcı onayladı), own_initiative (asistan kendisi önemli buldu).","parameters":{"type":"object","properties":{"text":{"type":"string"},"source":{"type":"string","enum":["user_asked","user_confirmed","own_initiative"]}},"required":["text","source"],"additionalProperties":false}},
{"type":"function","strict":false,"name":"forget","description":"Hafızadan not siler. query: notun içinde geçen bir kelime (ör. 'Pamuk'), '#numara' ya da 'all' (hepsi; yalnızca kullanıcı açıkça hepsini silmek isterse).","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false}},
{"type":"function","strict":false,"name":"set_timer","description":"Zamanlayıcı kurar: belirtilen dakika sonra cihaz zil çalar ve asistan etiketi kullanıcıya söyler. Yarım saat = 30, 90 saniye = 1.5.","parameters":{"type":"object","properties":{"minutes":{"type":"number","minimum":0.1,"maximum":43200},"label":{"type":"string"}},"required":["minutes","label"],"additionalProperties":false}},
{"type":"function","strict":false,"name":"set_alarm","description":"Belirli bir saate alarm ya da hatırlatıcı kurar (Türkiye saati). time: 'HH:MM' 24 saat. date: 'YYYY-MM-DD'; kullanıcı gün söylemediyse verme (saat bugün geçtiyse yarına kurulur).","parameters":{"type":"object","properties":{"time":{"type":"string"},"date":{"type":"string"},"label":{"type":"string"}},"required":["time","label"],"additionalProperties":false}},
{"type":"function","strict":false,"name":"list_timers","description":"Kurulu zamanlayıcı ve alarmları listeler.","parameters":{"type":"object","properties":{},"additionalProperties":false}},
{"type":"function","strict":false,"name":"cancel_timer","description":"Zamanlayıcı ya da alarmı iptal eder. query: etiketinden bir kelime, '#numara' ya da 'all'.","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false}}
])JSON";

namespace {

AracOrtam O;
uint32_t yerelSesAt = 0;
int yerelSesSeviye = -1;
bool yerelSesVar = false;

int sinirla(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

int32_t bugunYmd(int64_t simdi) {
  if (simdi <= 0) return 0;
  int y, m, d;
  hafiza::yerelTakvim(simdi, &y, &m, &d, nullptr, nullptr, nullptr);
  return y * 10000 + m * 100 + d;
}

// "HH:MM" / "H:MM" / "HH.MM" / "HH"
bool saatCoz(const char *s, int *h, int *m) {
  if (!s) return false;
  while (*s == ' ') s++;
  char *e = nullptr;
  long hh = strtol(s, &e, 10);
  if (e == s) return false;
  long mm = 0;
  if (*e == ':' || *e == '.') {
    const char *p = e + 1;
    mm = strtol(p, &e, 10);
    if (e == p) return false;
  }
  if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
  *h = (int)hh;
  *m = (int)mm;
  return true;
}

bool tarihCoz(const char *s, int *y, int *m, int *d) {
  if (!s || !*s) return false;
  int a = 0, b = 0, c = 0;
  if (sscanf(s, "%d-%d-%d", &a, &b, &c) != 3) return false;
  if (a < 2024 || b < 1 || b > 12 || c < 1 || c > 31) return false;
  *y = a; *m = b; *d = c;
  return true;
}

// Sesli komut için sözcük kökleri
bool basliyor(const char *t, const char *k) { return strncmp(t, k, strlen(k)) == 0; }
bool artirKok(const char *t) {
  if (!strcmp(t, "ac")) return true;
  static const char *const K[] = {"acar", "acsa", "acin", "acabil", "acmal", "artir", "yukselt", "fazlalastir", "yukari"};
  for (const char *k : K) if (basliyor(t, k)) return true;
  return false;
}
bool azaltKok(const char *t) {
  if (!strcmp(t, "kis")) return true;
  static const char *const K[] = {"kisar", "kissa", "kisin", "kisabil", "kismal", "azalt", "dusur", "alcalt", "asagi"};
  for (const char *k : K) if (basliyor(t, k)) return true;
  return false;
}
bool sesKelimesi(const char *t) {
  return !strcmp(t, "ses") || !strcmp(t, "sesi") || !strcmp(t, "sesini") || !strcmp(t, "sesin") || !strcmp(t, "sesimi");
}
bool sayi(const char *t) { return *t >= '0' && *t <= '9'; }
bool ayarFiili(const char *t) {
  return basliyor(t, "yap") || basliyor(t, "ayarla") || basliyor(t, "getir") || basliyor(t, "olsun") || basliyor(t, "cek");
}

void seviyeArgs(JsonDocument &a, int simdiki, int adim, int *yeni) {
  if (a["level"].is<int>()) { *yeni = sinirla(a["level"].as<int>()); return; }
  const char *ch = a["change"] | "";
  int st = a["step"] | adim;
  if (!strcmp(ch, "up")) *yeni = sinirla(simdiki + st);
  else if (!strcmp(ch, "down")) *yeni = sinirla(simdiki - st);
  else *yeni = -1;
}

}  // namespace

void aracBegin(const AracOrtam &o) { O = o; }

void aracYerelSesYapildi(int yeniSeviye) {
  yerelSesVar = true;
  yerelSesAt = O.ms ? O.ms() : 0;
  yerelSesSeviye = yeniSeviye;
}

int sesliSesKomutu(const char *metin, int simdiki) {
  char k[240];
  hafiza::katla(metin ? metin : "", k, sizeof(k));
  const char *tok[40];
  int n = 0;
  for (char *p = k; *p && n < 40;) {
    while (*p == ' ') p++;
    if (!*p) break;
    tok[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
  }
  bool biraz = false, cok = false;
  for (int i = 0; i < n; i++) {
    if (!strcmp(tok[i], "biraz") || !strcmp(tok[i], "azicik")) biraz = true;
    if (!strcmp(tok[i], "cok") || !strcmp(tok[i], "epey") || !strcmp(tok[i], "baya") || !strcmp(tok[i], "bayagi")) cok = true;
  }
  for (int i = 0; i < n; i++) {
    if (!sesKelimesi(tok[i])) continue;
    int son = i + 5 < n ? i + 5 : n;
    bool artir = false, azalt = false;
    int seviye = -1;
    for (int j = i + 1; j < son; j++) {
      const char *t = tok[j];
      if (!strcmp(t, "yuzde") && j + 1 < n && sayi(tok[j + 1])) { seviye = atoi(tok[j + 1]); break; }
      if (sayi(t) && j + 1 < n && ayarFiili(tok[j + 1])) { seviye = atoi(t); break; }
      if (!strcmp(t, "sonuna") || !strcmp(t, "fullle") || !strcmp(t, "full")) seviye = 100;
      if (artirKok(t)) artir = true;
      if (azaltKok(t)) azalt = true;
    }
    if (seviye >= 0) {
      if (seviye == 100 && azalt) continue;       // "sonuna kadar kıs" gibi tuhaf bir cümle: anlamadık say
      return sinirla(seviye);
    }
    if (artir == azalt) continue;                 // fiil yok ya da ikisi birden: komut değil
    int adim = biraz ? SESLI_SES_ADIM / 2 : (cok ? SESLI_SES_ADIM * 3 / 2 : SESLI_SES_ADIM);
    return sinirla(simdiki + (artir ? adim : -adim));
  }
  return -1;
}

const char *aracEtiketi(const char *ad) {
  static const char *const T[][2] = {
    {"set_volume", "sesi ayarlıyor"},   {"set_brightness", "ışığı ayarlıyor"},
    {"device_status", "kendine bakıyor"}, {"change_setting", "ayar değiştiriyor"},
    {"remember", "not alıyor"},         {"forget", "not siliyor"},
    {"set_timer", "zamanlayıcı kuruyor"}, {"set_alarm", "alarm kuruyor"},
    {"list_timers", "alarmlara bakıyor"}, {"cancel_timer", "alarm siliyor"},
  };
  if (!ad) return "";
  for (auto &e : T)
    if (!strcmp(ad, e[0])) return e[1];
  return "";
}

void aracCalistir(const char *ad, const char *argsJson, char *out, size_t cap) {
  JsonDocument a;
  if (deserializeJson(a, argsJson && *argsJson ? argsJson : "{}")) a.to<JsonObject>();
  int64_t simdi = O.simdi ? O.simdi() : 0;

  if (!strcmp(ad, "set_volume")) {
    int sim = O.ses ? O.ses() : 0, yeni = -1;
    uint32_t ms = O.ms ? O.ms() : 0;
    if (yerelSesVar && ms - yerelSesAt < 20000) {
      yerelSesVar = false;
      snprintf(out, cap, "Ses zaten %%%d yapıldı (cihaz kullanıcının sözünden anlayıp hemen ayarladı). Yeniden değiştirme.", yerelSesSeviye);
      return;
    }
    seviyeArgs(a, sim, 10, &yeni);
    if (yeni < 0) { snprintf(out, cap, "Hata: level ya da change gerekli. Şu anki ses %%%d.", sim); return; }
    if (O.sesAyarla) O.sesAyarla(yeni);
    snprintf(out, cap, "Ses %%%d yapıldı (önce %%%d).", yeni, sim);
    return;
  }
  if (!strcmp(ad, "set_brightness")) {
    int sim = O.parlaklik ? O.parlaklik() : 0, yeni = -1;
    seviyeArgs(a, sim, 20, &yeni);
    if (yeni < 0) { snprintf(out, cap, "Hata: level ya da change gerekli. Şu anki parlaklık %%%d.", sim); return; }
    if (yeni < 5) yeni = 5;                       // ekran tümden kararmasın: kullanıcı nereye konuştuğunu görsün
    if (O.parlaklikAyarla) O.parlaklikAyarla(yeni);
    snprintf(out, cap, "Ekran parlaklığı %%%d yapıldı (önce %%%d).", yeni, sim);
    return;
  }
  if (!strcmp(ad, "device_status")) {
    if (O.durum) O.durum(out, cap);
    else snprintf(out, cap, "Durum bilgisi yok.");
    return;
  }
  if (!strcmp(ad, "change_setting")) {
    const char *ayar = a["setting"] | "";
    const char *deger = a["value"] | "";
    if (!O.ayar || !O.ayar(ayar, deger, out, cap)) { if (!out[0]) snprintf(out, cap, "Hata: '%s' ayarı değiştirilemedi.", ayar); }
    return;
  }
  if (!strcmp(ad, "remember")) {
    const char *metin = a["text"] | "";
    const char *src = a["source"] | "own_initiative";
    char k = !strcmp(src, "user_asked") ? hafiza::KAYNAK_KULLANICI
             : !strcmp(src, "user_confirmed") ? hafiza::KAYNAK_ONAYLI : hafiza::KAYNAK_NOVA;
    char msg[300];
    int id = hafiza::ekle(metin, k, bugunYmd(simdi), msg, sizeof(msg));
    snprintf(out, cap, "%s%s", id > 0 ? "Hafızaya " : "Hata: ", msg);
    return;
  }
  if (!strcmp(ad, "forget")) {
    const char *q = a["query"] | "";
    if (!strcmp(q, "all")) q = "hepsi";
    char msg[600];
    int n = hafiza::unut(q, msg, sizeof(msg));
    snprintf(out, cap, "%s%s", n > 0 ? "Hafızadan " : "Silinmedi: ", msg);
    return;
  }
  if (!strcmp(ad, "set_timer") || !strcmp(ad, "set_alarm")) {
    if (simdi <= 0) { snprintf(out, cap, "Hata: cihazın saati henüz internetten alınmadı; biraz sonra yeniden dene."); return; }
    const char *etiket = a["label"] | "";
    int64_t vakit = 0;
    char tur = hafiza::ZAMANLAYICI;
    if (!strcmp(ad, "set_timer")) {
      double dk = a["minutes"] | 0.0;
      if (!(dk > 0)) { snprintf(out, cap, "Hata: minutes gerekli (dakika)."); return; }
      int64_t sn = (int64_t)llround(dk * 60.0);
      if (sn < 5) sn = 5;
      vakit = simdi + sn;
    } else {
      tur = hafiza::ALARM;
      int h, m, y, mo, d;
      if (!saatCoz(a["time"] | "", &h, &m)) { snprintf(out, cap, "Hata: time 'SS:DD' biçiminde olmalı."); return; }
      if (tarihCoz(a["date"] | "", &y, &mo, &d)) {
        vakit = hafiza::yerelEpoch(y, mo, d, h, m);
      } else {
        hafiza::yerelTakvim(simdi, &y, &mo, &d, nullptr, nullptr, nullptr);
        vakit = hafiza::yerelEpoch(y, mo, d, h, m);
        if (vakit <= simdi) vakit += 86400;      // saat bugün geçtiyse yarın
      }
    }
    char msg[200];
    int id = hafiza::kur(vakit, tur, etiket, simdi, msg, sizeof(msg));
    snprintf(out, cap, "%s%s", id > 0 ? (tur == hafiza::ALARM ? "Alarm " : "Zamanlayıcı ") : "Hata: ", msg);
    return;
  }
  if (!strcmp(ad, "list_timers")) {
    char liste[700];
    if (!hafiza::zamanlayicilar(liste, sizeof(liste), simdi)) snprintf(out, cap, "Kurulu zamanlayıcı ya da alarm yok.");
    else snprintf(out, cap, "Kurulu: %s", liste);
    return;
  }
  if (!strcmp(ad, "cancel_timer")) {
    const char *q = a["query"] | "";
    if (!strcmp(q, "all")) q = "hepsi";
    char msg[300];
    int n = hafiza::iptal(q, msg, sizeof(msg));
    snprintf(out, cap, "%s%s", n > 0 ? "" : "İptal edilmedi: ", msg);
    return;
  }
  snprintf(out, cap, "Hata: bu cihazda '%s' diye bir araç yok.", ad);
}
