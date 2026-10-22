#include "gundem.h"
#include "config.h"
#include "psbuf.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifndef NOVA_HOST_TEST
#include <Arduino.h>
#include "agistek.h"
#include "secrets.h"
#endif

namespace {

// Kullanıcının tarifi (18 Ekim): "genel en çarpıcı olaylar, gerçekten önemli ve güncel teknolojik gelişmeler"
const char TALIMAT[] =
    "Bir sesli asistan için günlük gündem özeti hazırlıyorsun. Web'de arayıp son 24 saatin haberlerinden şunları "
    "Türkçe derle:\n"
    "Genel: Türkiye'den ve dünyadan en çarpıcı, gerçekten önemli 3-4 olay.\n"
    "Teknoloji: gerçekten önemli ve yeni teknoloji gelişmeleri (yapay zekâ, çipler, büyük ürün çıkışları, uzay ve "
    "bilim), 2-3 madde.\n"
    "Kurallar: Her madde tek kısa cümle ve somut olsun (kim, ne, varsa sayı). Magazin, rutin açıklamalar, reklam, "
    "tahmin ve yorum yazma. Doğrulanmamış ya da tek zayıf kaynağa dayanan haberi alma. Bağlantı, kaynak ya da site "
    "adı, parantez içi kaynak, markdown ve emoji yazma. Toplam 900 karakteri geçme. Biçim tam olarak şöyle olsun:\n"
    "Genel:\n- ...\nTeknoloji:\n- ...";

int64_t vakit = 0;
char *metin = nullptr;           // PSRAM, GUNDEM_CAP + 1

bool alanAdiMi(const char *p, size_t n) {          // "reuters.com", "www.bbc.co.uk" gibi; "3.5" değil
  if (!n) return false;
  size_t nokta = n;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)p[i];
    if (!isalnum(ch) && ch != '.' && ch != '-' && ch != '/') return false;
    if (ch == '.') nokta = i;
  }
  if (nokta == n || nokta == 0 || n - nokta - 1 < 2) return false;
  for (size_t i = nokta + 1; i < n; i++)
    if (!isalpha((unsigned char)p[i]) && p[i] != '/') return false;
  return true;
}

size_t kopyalaKes(char *out, size_t cap, const char *s) {    // UTF-8 harfini bölmeden keser
  size_t n = strlen(s);
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  }
  memcpy(out, s, n);
  out[n] = 0;
  return n;
}

}  // namespace

size_t gundemTemizle(char *s) {
  // 1. geçiş: bağlantılar, adresler, ** işaretleri
  char *r = s, *w = s;
  while (*r) {
    if (*r == '[') {
      char *kap = strchr(r + 1, ']');
      if (kap && kap[1] == '(') {
        char *son = strchr(kap + 2, ')');
        if (son) {
          size_t el = (size_t)(kap - r - 1);
          if (!alanAdiMi(r + 1, el)) {                 // anlamlı etiket kalır, site adı gider
            memmove(w, r + 1, el);
            w += el;
          }
          r = son + 1;
          continue;
        }
      }
    }
    if (*r == '(') {                                    // "(reuters.com)" gibi parantez içi kaynak
      const char *kap = strchr(r + 1, ')');
      if (kap && kap - r <= 80 && alanAdiMi(r + 1, (size_t)(kap - r - 1))) {
        r = (char *)kap + 1;
        continue;
      }
    }
    if (!strncmp(r, "http://", 7) || !strncmp(r, "https://", 8) || !strncmp(r, "www.", 4)) {
      while (*r && *r != ' ' && *r != '\n' && *r != ')') r++;
      continue;
    }
    if (r[0] == '*' && r[1] == '*') { r += 2; continue; }
    if (*r == '#' && (r == s || r[-1] == '\n')) {      // başlık işareti
      while (*r == '#' || *r == ' ') r++;
      continue;
    }
    *w++ = *r++;
  }
  *w = 0;
  // 2. geçiş: boş kalan parantezler
  r = w = s;
  while (*r) {
    if (*r == '(') {
      const char *q = r + 1;
      while (*q == ' ') q++;
      if (*q == ')') { r = (char *)q + 1; continue; }
    }
    *w++ = *r++;
  }
  *w = 0;
  // 3. geçiş: noktalamadan önceki ve fazla boşluklar, satır başı ve sonu boşlukları
  r = w = s;
  while (*r) {
    if (*r == ' ') {
      const char *q = r;
      while (*q == ' ') q++;
      bool satirBasi = (w == s || w[-1] == '\n');
      if (!*q || *q == '\n' || *q == '.' || *q == ',' || *q == ';' || *q == ':' || *q == '!' || *q == '?' || *q == ')' || satirBasi) {
        r = (char *)q;
        continue;
      }
      *w++ = ' ';
      r = (char *)q;
      continue;
    }
    *w++ = *r++;
  }
  *w = 0;
  // 4. geçiş: boş satırlar ve yalnızca "-" kalan satırlar atılır
  r = w = s;
  while (*r) {
    char *nl = strchr(r, '\n');
    size_t n = nl ? (size_t)(nl - r) : strlen(r);
    bool bos = n == 0 || (n == 1 && *r == '-');
    if (!bos) {
      if (w != s) *w++ = '\n';
      memmove(w, r, n);
      w += n;
    }
    r += n + (nl ? 1 : 0);
  }
  *w = 0;
  return (size_t)(w - s);
}

size_t gundemIstekGovdesi(const char *model, const char *tarih, char *out, size_t cap) {
  JsonDocument d;
  d["model"] = model;
  d["background"] = true;
  d["reasoning"]["effort"] = "low";
  d["max_output_tokens"] = 3000;
  d["max_tool_calls"] = GUNDEM_MAX_ARAMA;
  JsonObject ws = d["tools"].add<JsonObject>();
  ws["type"] = "web_search";
  JsonObject yer = ws["user_location"].to<JsonObject>();
  yer["type"] = "approximate";
  yer["country"] = "TR";
  yer["city"] = HAVA_YER;
  yer["timezone"] = "Europe/Istanbul";
  d["instructions"] = TALIMAT;
  char giris[160];
  snprintf(giris, sizeof(giris), "Bugün: %s (Türkiye). Gündem özetini hazırla.", tarih ? tarih : "");
  d["input"] = giris;
  size_t n = measureJson(d);
  if (n + 1 > cap) return 0;
  return serializeJson(d, out, cap);
}

bool gundemCevapCoz(const char *json, size_t len, GundemCevap *c, char *ozet, size_t cap, char *hata, size_t hcap) {
  c->durum[0] = 0;
  c->id[0] = 0;
  c->metinVar = false;
  if (ozet && cap) ozet[0] = 0;
  JsonDocument filtre;
  filtre["id"] = true;
  filtre["status"] = true;
  filtre["error"]["message"] = true;
  filtre["incomplete_details"]["reason"] = true;
  filtre["output"][0]["type"] = true;
  filtre["output"][0]["content"][0]["type"] = true;
  filtre["output"][0]["content"][0]["text"] = true;
  JsonDocument d;
  if (deserializeJson(d, json, len, DeserializationOption::Filter(filtre))) { snprintf(hata, hcap, "cevap JSON değil"); return false; }
  kopyalaKes(c->id, sizeof(c->id), d["id"] | "");
  kopyalaKes(c->durum, sizeof(c->durum), d["status"] | "");
  if (!c->id[0] || !c->durum[0]) {
    snprintf(hata, hcap, "cevapta kimlik ya da durum yok%s%s", d["error"]["message"].is<const char *>() ? ": " : "",
             d["error"]["message"] | "");
    return false;
  }
  const char *m = nullptr;
  JsonArray cikti = d["output"];
  for (JsonObject o : cikti) {
    if (strcmp(o["type"] | "", "message")) continue;
    JsonArray icerik = o["content"];
    for (JsonObject ic : icerik)
      if (!strcmp(ic["type"] | "", "output_text")) m = ic["text"] | (const char *)nullptr;
  }
  if (m && *m && ozet && cap > 1) {
    kopyalaKes(ozet, cap, m);
    gundemTemizle(ozet);
    c->metinVar = ozet[0] != 0;
  }
  if (strcmp(c->durum, "completed") && strcmp(c->durum, "queued") && strcmp(c->durum, "in_progress"))
    snprintf(hata, hcap, "durum: %s%s%s%s%s", c->durum, d["error"]["message"].is<const char *>() ? ", " : "",
             d["error"]["message"] | "", d["incomplete_details"]["reason"].is<const char *>() ? ", sebep: " : "",
             d["incomplete_details"]["reason"] | "");
  return true;
}

bool gundemZamani(int64_t sonCekis, int64_t simdi, int32_t tz) {
  static const int SAAT[] = GUNDEM_SAATLER;
  const int N = (int)(sizeof(SAAT) / sizeof(SAAT[0]));
  if (simdi <= 0 || N == 0) return false;
  if (sonCekis <= 0) return true;
  int64_t yerel = simdi + tz;
  int64_t gunBasi = yerel - (yerel % 86400);
  int64_t son = -1;                                    // geçmişteki en yakın gündem saati (yerel)
  for (int i = 0; i < N; i++) {
    int64_t t = gunBasi + (int64_t)SAAT[i] * 3600;
    if (t <= yerel && t > son) son = t;
  }
  if (son < 0) {                                       // bugünün ilk saatinden önce: dünün sonuncusu
    int enGec = 0;
    for (int i = 0; i < N; i++) if (SAAT[i] > enGec) enGec = SAAT[i];
    son = gunBasi - 86400 + (int64_t)enGec * 3600;
  }
  return sonCekis < son - tz;
}

void gundemSakla(const char *ozet, int64_t v) {
  if (!ozet || !psLazy(metin, GUNDEM_CAP + 1)) return;
  kopyalaKes(metin, GUNDEM_CAP + 1, ozet);
  vakit = metin[0] ? v : 0;
}

int64_t gundemVakti() { return vakit; }
const char *gundemMetni() { return metin ? metin : ""; }

size_t gundemYazi(char *out, size_t cap, int64_t simdi, int32_t tz) {
  if (!cap) return 0;
  out[0] = 0;
  if (!metin || !metin[0] || !vakit || simdi <= 0 || simdi - vakit > (int64_t)GUNDEM_ESKI_SAAT * 3600 || simdi < vakit - 300)
    return 0;
  int64_t y = vakit + tz, s = simdi + tz;
  int saat = (int)((y % 86400) / 3600), dk = (int)((y % 3600) / 60);
  const char *gun = (y / 86400 == s / 86400) ? "bugün" : "dün";
  int n = snprintf(out, cap, "Gündem (%s saat %02d:%02d itibarıyla):\n%s", gun, saat, dk, metin);
  if (n < 0) { out[0] = 0; return 0; }
  return (size_t)n < cap ? (size_t)n : cap - 1;
}

#ifndef NOVA_HOST_TEST
namespace {
char bekleyenId[96] = "";
int yoklama = 0;
char *cevapBuf = nullptr;
const size_t CEVAP_CAP = 48 * 1024;

int istek(const char *yontem, const char *yol, const char *govde, size_t gl, char **cevap, size_t *cl, char *hata, size_t hcap) {
  char basliklar[300];
  snprintf(basliklar, sizeof(basliklar), "Authorization: Bearer %s\r\n%s", OPENAI_API_KEY,
           govde ? "Content-Type: application/json\r\n" : "");
  return httpsIstek("api.openai.com", yontem, yol, basliklar, govde, gl, cevapBuf, CEVAP_CAP, cevap, cl, 20000, hata, hcap);
}
}  // namespace

bool gundemBekliyor() { return bekleyenId[0] != 0; }

int gundemBaslat(int64_t simdi, const char *tarih, char *hata, size_t hcap) {
  static char *govde = nullptr, *ozet = nullptr;
  const size_t GC = 4096;
  if (!psLazy(cevapBuf, CEVAP_CAP) || !psLazy(govde, GC) || !psLazy(ozet, GUNDEM_CAP + 1)) { snprintf(hata, hcap, "bellek yok"); return -1; }
  size_t gl = gundemIstekGovdesi(GPTLIVE_BACKEND_MODEL, tarih, govde, GC);
  if (!gl) { snprintf(hata, hcap, "istek sığmadı"); return -1; }
  char *cevap = nullptr;
  size_t cl = 0;
  int kod = istek("POST", "/v1/responses", govde, gl, &cevap, &cl, hata, hcap);
  if (kod < 0) return 0;
  if (kod != 200) { snprintf(hata, hcap, "HTTP %d: %.200s", kod, cevap ? cevap : ""); return -1; }
  GundemCevap c;
  if (!gundemCevapCoz(cevap, cl, &c, ozet, GUNDEM_CAP + 1, hata, hcap)) return -1;
  if (!strcmp(c.durum, "queued") || !strcmp(c.durum, "in_progress")) {
    kopyalaKes(bekleyenId, sizeof(bekleyenId), c.id);
    yoklama = 0;
    return 1;
  }
  if (c.metinVar && !strcmp(c.durum, "completed")) { gundemSakla(ozet, simdi); return 1; }   // hemen bitti
  if (!hata[0]) snprintf(hata, hcap, "beklenmeyen durum: %s", c.durum);
  return -1;
}

int gundemYokla(int64_t simdi, char *hata, size_t hcap) {
  static char *ozet = nullptr;
  if (!bekleyenId[0]) return -1;
  if (!psLazy(cevapBuf, CEVAP_CAP) || !psLazy(ozet, GUNDEM_CAP + 1)) { snprintf(hata, hcap, "bellek yok"); bekleyenId[0] = 0; return -1; }
  if (++yoklama > GUNDEM_YOKLA_MAX) {
    snprintf(hata, hcap, "%d yoklamada bitmedi", GUNDEM_YOKLA_MAX);
    bekleyenId[0] = 0;
    return -1;
  }
  char yol[128];
  snprintf(yol, sizeof(yol), "/v1/responses/%s", bekleyenId);
  char *cevap = nullptr;
  size_t cl = 0;
  hata[0] = 0;
  int kod = istek("GET", yol, nullptr, 0, &cevap, &cl, hata, hcap);
  if (kod < 0) return 0;
  if (kod != 200) { snprintf(hata, hcap, "HTTP %d: %.200s", kod, cevap ? cevap : ""); bekleyenId[0] = 0; return -1; }
  GundemCevap c;
  if (!gundemCevapCoz(cevap, cl, &c, ozet, GUNDEM_CAP + 1, hata, hcap)) { bekleyenId[0] = 0; return -1; }
  if (!strcmp(c.durum, "queued") || !strcmp(c.durum, "in_progress")) {
    snprintf(hata, hcap, "sürüyor (%s, %d. yoklama)", c.durum, yoklama);
    return 0;
  }
  bekleyenId[0] = 0;
  if (c.metinVar && (!strcmp(c.durum, "completed") || !strcmp(c.durum, "incomplete"))) {
    gundemSakla(ozet, simdi);
    return 1;
  }
  if (!hata[0]) snprintf(hata, hcap, "metin yok (durum: %s)", c.durum);
  return -1;
}
#endif
