#include "notcikar.h"
#include "hafiza.h"
#include "config.h"
#include "psbuf.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <string.h>
#include <stdio.h>

#ifndef NOVA_HOST_TEST
#include <Arduino.h>
#include "agistek.h"
#include "secrets.h"
#endif

namespace {

const size_t KONUSMA_CAP = 8192;
char *konusma = nullptr;          // biriken (PSRAM)
size_t konusmaN = 0;
size_t kullaniciHarf = 0;
char *bekleyen = nullptr;         // işlenmeyi bekleyen (PSRAM)
bool bekliyor = false;
int deneme = 0;

const char TALIMAT[] =
    "Sen bir sesli asistanın hafıza yöneticisisin. Sana kullanıcının mevcut kalıcı notları (numaralı) ve az önce biten "
    "sesli konuşmanın yazısı verilecek; yazıya dökmede hatalar olabilir. Görevin, konuşmadan kalıcı olarak saklanması "
    "gereken yeni bilgileri ve silinmesi gereken notları çıkarmak.\n"
    "Kurallar:\n"
    "- Kullanıcı açıkça hatırlamanı istediyse ('hatırla', 'not al', 'aklında tut', 'hafızana yaz' gibi) kaydet; kaynak user_asked.\n"
    "- Asistan 'hatırlayayım mı' diye sorduysa ve kullanıcı evet dediyse kaydet; kaynak user_confirmed.\n"
    "- Kullanıcı hakkında kalıcı ve işe yarar bir bilgi geçtiyse (tercihleri, alışkanlıkları, yakınlarının ve evcil "
    "hayvanlarının adları, düzenli işleri, devam eden projeleri) kaydet; kaynak own_initiative.\n"
    "- Sağlık, ilişkiler, para ya da başkalarının özel bilgileri gibi hassas konuları yalnızca kullanıcı açıkça "
    "istediyse ya da onayladıysa kaydet.\n"
    "- Geçici şeyleri (o anki ruh hali, bugünkü işler, sorulan sorular, hava durumu, asistanın kendi söyledikleri), "
    "selamlaşmayı ve genel bilgiyi kaydetme.\n"
    "- Mevcut notlarda zaten olan bilgiyi farklı kelimelerle de olsa yeniden ekleme.\n"
    "- Her not üçüncü şahısla, Türkçe, tek kısa cümle olsun (ör. 'Kullanıcının köpeğinin adı Karabaş.'). Birden "
    "fazla ayrı bilgiyi ayrı notlar olarak yaz.\n"
    "- Kullanıcı bir şeyi unutmanı istediyse ya da bir not artık yanlışsa, o notun numarasını '#3' biçiminde sil "
    "listesine koy; değişen bilgi için yenisini ekle.\n"
    "- Kaydedilecek ya da silinecek bir şey yoksa iki listeyi de boş döndür.";

const char SEMA[] =
    "{\"type\":\"object\",\"properties\":{\"ekle\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":"
    "{\"metin\":{\"type\":\"string\"},\"kaynak\":{\"type\":\"string\",\"enum\":[\"user_asked\",\"user_confirmed\","
    "\"own_initiative\"]}},\"required\":[\"metin\",\"kaynak\"],\"additionalProperties\":false}},\"sil\":{\"type\":"
    "\"array\",\"items\":{\"type\":\"string\"}}},\"required\":[\"ekle\",\"sil\"],\"additionalProperties\":false}";

// JSON yazısı olarak kaçışlı ekler
size_t jsonYazi(char *out, size_t cap, size_t o, const char *s) {
  if (o + 2 >= cap) return cap;
  out[o++] = '"';
  for (; *s && o + 7 < cap; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
    else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
    else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
    else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
    else out[o++] = (char)c;
  }
  if (*s || o + 2 >= cap) return cap;          // sığmadı
  out[o++] = '"';
  out[o] = 0;
  return o;
}

size_t ekleDuz(char *out, size_t cap, size_t o, const char *s) {
  size_t n = strlen(s);
  if (o + n + 1 >= cap) return cap;
  memcpy(out + o, s, n + 1);
  return o + n;
}

}  // namespace

void notKonusmaEkle(bool kullanici, const char *metin) {
  if (!metin || !*metin || !psLazy(konusma, KONUSMA_CAP)) return;
  char satir[700];
  int n = snprintf(satir, sizeof(satir), "%s: %s\n", kullanici ? "Kullanıcı" : "Nova", metin);
  if (n <= 0) return;
  size_t len = (size_t)n < sizeof(satir) ? (size_t)n : sizeof(satir) - 1;
  if (len >= KONUSMA_CAP) return;
  if (konusmaN + len >= KONUSMA_CAP) {          // sığmıyor: en eski satırlar atılır
    size_t at = konusmaN + len - (KONUSMA_CAP - 1);
    const char *nl = (const char *)memchr(konusma + at, '\n', konusmaN - at);
    at = nl ? (size_t)(nl - konusma) + 1 : konusmaN;
    memmove(konusma, konusma + at, konusmaN - at);
    konusmaN -= at;
  }
  memcpy(konusma + konusmaN, satir, len);
  konusmaN += len;
  konusma[konusmaN] = 0;
  if (kullanici) kullaniciHarf += strlen(metin);
}

bool notOturumBitti() {
  bool yeterli = konusma && kullaniciHarf >= 20;
  if (yeterli && psLazy(bekleyen, KONUSMA_CAP)) {
    if (bekliyor) {                              // öncekisi işlenmeden yenisi geldi: arkasına eklenir
      size_t a = strlen(bekleyen);
      size_t yer = KONUSMA_CAP - 1 - a;
      size_t n = konusmaN < yer ? konusmaN : yer;
      memcpy(bekleyen + a, konusma, n);
      bekleyen[a + n] = 0;
    } else {
      memcpy(bekleyen, konusma, konusmaN + 1);
    }
    bekliyor = true;
    deneme = 0;
  }
  konusmaN = 0;
  kullaniciHarf = 0;
  if (konusma) konusma[0] = 0;
  return yeterli;
}

bool notBekliyor() { return bekliyor; }

size_t notIstekGovdesi(const char *model, const char *notlar, const char *konusmaYazi, char *out, size_t cap) {
  size_t o = 0;
  o = ekleDuz(out, cap, o, "{\"model\":");
  o = jsonYazi(out, cap, o, model);
  o = ekleDuz(out, cap, o, ",\"store\":false,\"reasoning\":{\"effort\":\"low\"},\"max_output_tokens\":4000,\"instructions\":");
  o = jsonYazi(out, cap, o, TALIMAT);
  o = ekleDuz(out, cap, o, ",\"input\":");
  // Mevcut notlar + konuşma tek kullanıcı mesajında
  static const char *const BAS[2] = {"Mevcut notlar:\n", "\nKonuşma:\n"};
  if (o >= cap) return 0;
  size_t ic = 0;
  static char *birlesik = nullptr;
  const size_t BC = 24 * 1024;
  if (!psLazy(birlesik, BC)) return 0;
  ic = ekleDuz(birlesik, BC, ic, BAS[0]);
  ic = ekleDuz(birlesik, BC, ic, notlar && *notlar ? notlar : "(yok)\n");
  ic = ekleDuz(birlesik, BC, ic, BAS[1]);
  ic = ekleDuz(birlesik, BC, ic, konusmaYazi ? konusmaYazi : "");
  if (ic >= BC) return 0;
  o = jsonYazi(out, cap, o, birlesik);
  o = ekleDuz(out, cap, o, ",\"text\":{\"format\":{\"type\":\"json_schema\",\"name\":\"hafiza_guncelleme\",\"strict\":true,\"schema\":");
  o = ekleDuz(out, cap, o, SEMA);
  o = ekleDuz(out, cap, o, "}}}");
  return o >= cap ? 0 : o;
}

int notCevabiUygula(const char *json, size_t len, int32_t ymd, char *rapor, size_t cap) {
  JsonDocument filtre;
  filtre["output"][0]["type"] = true;
  filtre["output"][0]["content"][0]["type"] = true;
  filtre["output"][0]["content"][0]["text"] = true;
  filtre["status"] = true;
  filtre["error"]["message"] = true;
  JsonDocument d;
  if (deserializeJson(d, json, len, DeserializationOption::Filter(filtre))) { snprintf(rapor, cap, "cevap JSON değil"); return -1; }
  const char *metin = nullptr;
  JsonArray cikti = d["output"];
  for (JsonObject o : cikti) {
    if (strcmp(o["type"] | "", "message")) continue;
    JsonArray icerik = o["content"];
    for (JsonObject c : icerik)
      if (!strcmp(c["type"] | "", "output_text")) metin = c["text"] | (const char *)nullptr;
  }
  if (!metin) {
    snprintf(rapor, cap, "cevapta metin yok (durum: %s%s%s)", d["status"] | "?", d["error"]["message"].is<const char *>() ? ", " : "",
             d["error"]["message"] | "");
    return -1;
  }
  JsonDocument g;
  if (deserializeJson(g, metin)) { snprintf(rapor, cap, "modelin cevabı anlaşılmadı"); return -1; }
  int sayi = 0;
  size_t o = 0;
  rapor[0] = 0;
  char msg[300];
  JsonArray silinecek = g["sil"], eklenecek = g["ekle"];
  for (JsonVariant s : silinecek) {
    const char *q = s | "";
    if (!*q) continue;
    if (hafiza::unut(q, msg, sizeof(msg)) > 0) {
      sayi++;
      int n = snprintf(rapor + o, cap - o, "%s%s", o ? "; " : "", msg);
      if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    }
  }
  for (JsonObject e : eklenecek) {
    const char *m = e["metin"] | "";
    const char *k = e["kaynak"] | "own_initiative";
    char kk = !strcmp(k, "user_asked") ? hafiza::KAYNAK_KULLANICI
              : !strcmp(k, "user_confirmed") ? hafiza::KAYNAK_ONAYLI : hafiza::KAYNAK_NOVA;
    int id = hafiza::ekle(m, kk, ymd, msg, sizeof(msg));
    if (id > 0 && strncmp(msg, "zaten", 5)) {
      sayi++;
      int n = snprintf(rapor + o, cap - o, "%s%s", o ? "; " : "", msg);
      if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
    }
  }
  if (!sayi) snprintf(rapor, cap, "yeni not yok");
  return sayi;
}

#ifndef NOVA_HOST_TEST
bool notCikar(int32_t ymd, char *rapor, size_t cap) {
  if (!bekliyor) return true;
  const size_t GC = 32 * 1024, CC = 24 * 1024;
  static char *govde = nullptr, *cevapBuf = nullptr, *notlar = nullptr;
  if (!psLazy(govde, GC) || !psLazy(cevapBuf, CC) || !psLazy(notlar, 16 * 1024)) { snprintf(rapor, cap, "bellek yok"); bekliyor = false; return true; }
  hafiza::numaraliListe(notlar, 16 * 1024);
  size_t gl = notIstekGovdesi(GPTLIVE_BACKEND_MODEL, notlar, bekleyen, govde, GC);
  if (!gl) { snprintf(rapor, cap, "istek sığmadı"); bekliyor = false; return true; }
  char basliklar[300];
  snprintf(basliklar, sizeof(basliklar), "Authorization: Bearer %s\r\nContent-Type: application/json\r\n", OPENAI_API_KEY);
  char *cevap = nullptr;
  size_t cl = 0;
  char hata[120] = "";
  int kod = httpsIstek("api.openai.com", "POST", "/v1/responses", basliklar, govde, gl, cevapBuf, CC, &cevap, &cl, 30000,
                       hata, sizeof(hata));
  if (kod < 0) {
    snprintf(rapor, cap, "%s", hata);
    if (++deneme >= 3) { bekliyor = false; return true; }    // üç denemede olmadı: bu konuşmadan vazgeç
    return false;
  }
  bekliyor = false;
  if (kod != 200) {
    snprintf(rapor, cap, "HTTP %d: %.160s", kod, cevap ? cevap : "");
    return true;
  }
  notCevabiUygula(cevap, cl, ymd, rapor, cap);
  return true;
}
#endif
