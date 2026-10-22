#include "gptlive.h"
#include "b64.h"
#include "resample.h"
#include "psbuf.h"
#include "config.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef GPTLIVE_AUDIO_RATE
#define GPTLIVE_AUDIO_RATE 16000
#endif
#if GPTLIVE_AUDIO_RATE != 16000 && GPTLIVE_AUDIO_RATE != 24000
#error "GPTLIVE_AUDIO_RATE 16000 ya da 24000 olmali"
#endif

namespace gptlive {

namespace {

Upsampler16to24 up;
const size_t UP24_N = 2048 * 3 / 2;
int16_t *up24 = nullptr;      // 24 kHz'e çevrilmiş mikrofon çerçevesi (UP24_N örnek, PSRAM'de; yalnız 24 kHz biçimde)
uint8_t *audioBuf = nullptr;  // gelen sesin base64'ten çözüldüğü tampon (PSRAM'de)
// 16 kHz biçimde gelen ses hoparlör için 24 kHz'e çevrilir (hoparlör, sınırlayıcı ve yankı referansı 24 kHz'te kalır).
Upsampler16to24 upRx;
const size_t RX_BLK = 1536;   // bir seferde çevrilen en çok giriş örneği (çıkışta 96 ms)
int16_t *rx24 = nullptr;      // çevrilmiş parça (RX_BLK*3/2 örnek, PSRAM'de)
size_t rxHave = 0;            // önceki parçadan kalan (çift örneğe tamamlanmamış) bayt, audioBuf'ın başında
char pendingDelegation[80] = "";   // cihaza devredilen ve henüz cevaplanmamış iş
bool backendBusy = false;          // arka model çalışıyor (kayıt satırı bir kez yazılsın diye)

// Bu bağlantıda ilk kez görülen olay türleri Serial'e bir kez yazılır (ayrıntılı kayıt açıkken):
// bu API'yi gerçek sunucuda ilk kez görüyoruz, kayıttan ne yolladığı anlaşılsın.
bool logFirstEvents = false;      // gptlive::setFirstEventLog ile açılır (yeni bir sunucu davranışını incelerken)
uint32_t seenTypes[28];
int seenCount = 0;
bool audioSeen = false;

// Söz sırası ölçümü: sunucu yazı parçalarına oturumun zaman çizgisindeki yerini koyuyor.
// Kullanıcının son sözünün bitişi ile modelin ilk sözünün başlangıcı arasındaki fark, modelin
// tepki süresidir (ağ gecikmesi ve klavye tıkırtısı karışmaz). Eksi çıkarsa model söze girmiştir.
long userEndMs = -1;
bool lastWasUser = false;

// Aynı araç çağrısı iki kez gelirse (aynı call_id) bir kez çalıştırılır
char lastCalls[4][72];
int lastCallPos = 0;
bool seenCall(const char *id) {
  for (int i = 0; i < 4; i++) if (lastCalls[i][0] && !strcmp(lastCalls[i], id)) return true;
  strncpy(lastCalls[lastCallPos], id, sizeof(lastCalls[0]) - 1);
  lastCalls[lastCallPos][sizeof(lastCalls[0]) - 1] = 0;
  lastCallPos = (lastCallPos + 1) % 4;
  return false;
}
uint32_t eventSeq = 0;

bool firstSight(const char *a, const char *b) {
  uint32_t h = 2166136261u;
  for (const char *s = a; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  h = (h ^ '/') * 16777619u;
  for (const char *s = b; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  for (int i = 0; i < seenCount; i++) if (seenTypes[i] == h) return false;
  if (seenCount >= (int)(sizeof(seenTypes) / sizeof(seenTypes[0]))) return false;
  seenTypes[seenCount++] = h;
  return true;
}

inline bool isWs(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

// Üst düzey nesnedeki "key": "değer" çiftinin değerini bulur (açılış tırnağından sonrası) ve
// uzunluğunu verir. İç içe nesnelere ve yazıların içine bakmaz: zarf mesajlarında (response.event)
// içteki olayın da "type" alanı vardır ve alanların sırası belgelenmemiştir.
const char *topString(const char *msg, size_t len, const char *key, size_t *valLen) {
  const char *p = msg, *end = msg + len;
  size_t klen = strlen(key);
  int depth = 0;
  bool expectKey = false;
  while (p < end) {
    char c = *p;
    if (c == '"') {
      const char *s = ++p;
      while (p < end && *p != '"') { if (*p == '\\' && p + 1 < end) p++; p++; }
      const char *e = p;
      if (p < end) p++;
      if (depth == 1 && expectKey) {
        expectKey = false;
        const char *q = p;
        while (q < end && isWs(*q)) q++;
        if (q < end && *q == ':') {
          q++;
          while (q < end && isWs(*q)) q++;
          if ((size_t)(e - s) == klen && memcmp(s, key, klen) == 0) {
            if (q >= end || *q != '"') return nullptr;
            const char *v = q + 1, *w = v;
            while (w < end && *w != '"') { if (*w == '\\' && w + 1 < end) w++; w++; }
            *valLen = (size_t)(w - v);
            return v;
          }
          p = q;
        }
      }
      continue;
    }
    if (c == '{' || c == '[') { depth++; if (depth == 1 && c == '{') expectKey = true; }
    else if (c == '}' || c == ']') depth--;
    else if (c == ',' && depth == 1) expectKey = true;
    p++;
  }
  return nullptr;
}

bool typeIs(const char *ty, size_t tyLen, const char *name) {
  return strlen(name) == tyLen && memcmp(ty, name, tyLen) == 0;
}
bool typeStarts(const char *ty, size_t tyLen, const char *prefix) {
  size_t n = strlen(prefix);
  return tyLen >= n && memcmp(ty, prefix, n) == 0;
}

// 16 kHz biçim: base64'ü çözer, 24 kHz'e çevirip verir. Parçalar eşit boyda verilir (100 ms'lik ses
// 50+50 ms): link.cpp her parçanın seviyesine bakıp sessizlik mi konuşma mı diye karar veriyor, birkaç
// ms'lik artık parçanın seviyesi gürültülü olur.
size_t decodeAudio16(const char *d, size_t dLen, ModelSink &sink) {
  uint8_t *buf = psLazy(audioBuf, RX_BLK * 2 + 8);
  int16_t *o24 = psLazy(rx24, RX_BLK * 3 / 2);
  if (!buf || !o24) return 0;
  size_t estN = dLen / 4 * 3 / 2 + rxHave / 2;     // yaklaşık örnek sayısı
  size_t k = estN > RX_BLK ? (estN + RX_BLK - 1) / RX_BLK : 1;
  size_t blk = ((estN + k - 1) / k + 1) & ~(size_t)1;
  if (blk < 2) blk = 2;
  if (blk > RX_BLK) blk = RX_BLK;
  const char *end = d + dLen;
  B64Decoder dec;
  size_t total = 0;
  for (;;) {
    if (d < end && rxHave < blk * 2) {
      size_t used = 0;
      rxHave += dec.feed(d, (size_t)(end - d), buf + rxHave, blk * 2 - rxHave, &used);
      d += used;
    }
    bool done = d >= end;
    if (rxHave >= blk * 2 || (done && rxHave >= 4)) {
      size_t n = rxHave / 4 * 2;                     // çift sayıda örnek (çevirici böyle ister)
      size_t m = upRx.process((const int16_t *)buf, n, o24);
      sink.onAudio((const uint8_t *)o24, m * 2);
      total += m * 2;
      size_t rest = rxHave - n * 2;                  // 0-3 bayt: sonraki parçaya kalır
      memmove(buf, buf + n * 2, rest);
      rxHave = rest;
    }
    if (done) break;
  }
  return total;
}

// base64 sesi JSON ağacı kurmadan, parça parça çözer.
size_t decodeAudio(const char *d, size_t dLen, ModelSink &sink) {
  if (GPTLIVE_AUDIO_RATE == 16000) return decodeAudio16(d, dLen, sink);
  uint8_t *buf = psLazy(audioBuf, 3072 + 8);
  if (!buf) return 0;
  const char *end = d + dLen;
  B64Decoder dec;
  size_t have = 0, total = 0;
  while (d < end) {
    size_t seg = (size_t)(end - d) < 4096 ? (size_t)(end - d) : 4096;
    size_t used = 0;
    size_t n = dec.feed(d, seg, buf + have, 3072, &used);
    d += used;
    have += n;
    size_t even = have & ~(size_t)1;       // örnekler iki baytlık: hep çift sayıda bayt ver
    if (even) { sink.onAudio(buf, even); total += even; }
    if (have & 1) { buf[0] = buf[have - 1]; have = 1; } else { have = 0; }
    if (used == 0) break;
  }
  return total;
}

JsonDocument &filterDoc() {
  static JsonDocument f;
  static bool ready = false;
  if (!ready) {
    ready = true;
    f["type"] = true;
    f["delta"] = true;
    f["reason"] = true;
    f["usage"]["seconds"] = true;
    f["start_ms"] = true;
    f["end_ms"] = true;
    f["session"]["expires_at"] = true;
    JsonObject e = f["error"].to<JsonObject>();
    e["message"] = true;
    e["code"] = true;
    e["type"] = true;
    e["param"] = true;
    JsonObject dg = f["delegation"].to<JsonObject>();
    dg["id"] = true;
    dg["target"] = true;
    JsonObject ev = f["event"].to<JsonObject>();           // response.event zarfının içindeki olay
    ev["type"] = true;
    JsonObject it = ev["item"].to<JsonObject>();           // response.output_item.done: biten öğe (araç çağrısı)
    it["type"] = true;
    it["call_id"] = true;
    it["name"] = true;
    it["arguments"] = true;
    ev["response"]["status"] = true;
    ev["response"]["error"]["message"] = true;
    ev["message"] = true;
  }
  return f;
}

size_t appendEvent(char *out, size_t cap, const char *type, const char *eventId, const char *delegationId, const char *content) {
  JsonDocument d;
  d["type"] = type;
  d["event_id"] = eventId;
  if (delegationId && *delegationId) d["delegation_id"] = delegationId;
  else d["delegation_id"] = nullptr;                       // alan zorunlu; genel bağlam için null
  d["content"] = content;
  if (measureJson(d) + 1 > cap) return 0;
  return serializeJson(d, out, cap);
}

}  // namespace

void setFirstEventLog(bool on) { logFirstEvents = on; }

void reset() {
  up.reset();
  upRx.reset();
  rxHave = 0;
  pendingDelegation[0] = 0;
  backendBusy = false;
  seenCount = 0;
  audioSeen = false;
  userEndMs = -1;
  lastWasUser = false;
  memset(lastCalls, 0, sizeof(lastCalls));
  lastCallPos = 0;
}

size_t buildSetup(char *out, size_t cap, const ModelConfig &c) {
  // Sığmazsa geçmişin en eski satırları bırakılır.
  for (int skip = 0; skip <= (c.historyCount > 0 ? c.historyCount : 0); skip++) {
    JsonDocument d;
    d["type"] = "session.start";
    d["event_id"] = "baslat";
    JsonObject s = d["session"].to<JsonObject>();
    s["model"] = c.model;
    if (c.systemText && *c.systemText) s["instructions"] = c.systemText;
    JsonObject a = s["audio"].to<JsonObject>();
    // Giriş ve çıkış aynı biçimde, mono 16 bit. 16 kHz: ağda 24 kHz'in 2/3'ü kadar veri (2.8.8).
    a["format"]["type"] = "audio/pcm";
    a["format"]["rate"] = GPTLIVE_AUDIO_RATE;
    if (c.level <= 2 && c.voice && *c.voice) a["output"]["voice"] = c.voice;
    if (c.level <= 1 && c.history && c.historyCount > skip) {
      JsonArray in = s["input"].to<JsonArray>();
      for (int i = skip; i < c.historyCount; i++) {
        JsonObject it = in.add<JsonObject>();
        it["type"] = "message";
        it["role"] = c.history[i].user ? "user" : "assistant";
        JsonObject cp = it["content"].to<JsonArray>().add<JsonObject>();
        cp["type"] = c.history[i].user ? "input_text" : "output_text";
        cp["text"] = c.history[i].text;
      }
    }
    if (c.level <= 2 && c.backendModel && *c.backendModel) {
      JsonObject dg = s["delegation"].to<JsonObject>();
      dg["type"] = "responses";
      JsonObject r = dg["responses"].to<JsonObject>();
      r["model"] = c.backendModel;
      if (c.level <= 1) {
        if (c.backendNote && *c.backendNote) r["instructions"] = c.backendNote;
        JsonArray tools;
        if (c.webSearch) {
          tools = r["tools"].to<JsonArray>();
          tools.add<JsonObject>()["type"] = "web_search";
        }
        if (c.toolsJson && *c.toolsJson) {
          // Cihaz araçları (function): sabit JSON metninden kopyalanır
          JsonDocument td;
          if (!deserializeJson(td, c.toolsJson) && td.is<JsonArray>()) {
            if (tools.isNull()) tools = r["tools"].to<JsonArray>();
            for (JsonVariant t : td.as<JsonArray>()) tools.add(t);
          }
        }
        if (!tools.isNull()) r["tool_choice"] = "auto";
      }
      if (c.level == 0) {
        // Arka modeli hızlandıran ayarlar: kabul edilmezse merdivenin bir alt basamağı bunlarsız dener
        if (c.backendTier && *c.backendTier) r["service_tier"] = c.backendTier;
        if (c.backendEffort && *c.backendEffort) r["reasoning"]["effort"] = c.backendEffort;
        // Araçlar sırayla çağrılsın: her sonuçtan sonra arka model kaldığı yerden devam eder
        if (c.toolsJson && *c.toolsJson) r["parallel_tool_calls"] = false;
      }
    }
    if (measureJson(d) + 1 <= cap) return serializeJson(d, out, cap);
  }
  return 0;
}

size_t buildAudio(char *out, size_t cap, const int16_t *pcm16k, size_t samples) {
  static const char PRE[] = "{\"type\":\"session.input_audio.append\",\"audio\":\"";
  static const char POST[] = "\"}";
  if (GPTLIVE_AUDIO_RATE == 16000) {                       // mikrofon zaten 16 kHz: olduğu gibi gider
    size_t bytes = samples * 2;
    size_t need = sizeof(PRE) - 1 + 4 * ((bytes + 2) / 3) + sizeof(POST) - 1;
    if (need + 1 > cap) return 0;
    size_t o = sizeof(PRE) - 1;
    memcpy(out, PRE, o);
    o += b64Encode((const uint8_t *)pcm16k, bytes, out + o);
    memcpy(out + o, POST, sizeof(POST) - 1);
    o += sizeof(POST) - 1;
    out[o] = 0;
    return o;
  }
  if ((samples & 1) || samples * 3 / 2 > UP24_N || !psLazy(up24, UP24_N)) return 0;
  size_t bytes = samples * 3 / 2 * 2;
  size_t need = sizeof(PRE) - 1 + 4 * ((bytes + 2) / 3) + sizeof(POST) - 1;
  if (need + 1 > cap) return 0;
  up.process(pcm16k, samples, up24);
  size_t o = sizeof(PRE) - 1;
  memcpy(out, PRE, o);
  o += b64Encode((const uint8_t *)up24, bytes, out + o);    // ESP32 ve x86 küçük uçlu: PCM olduğu gibi gider
  memcpy(out + o, POST, sizeof(POST) - 1);
  o += sizeof(POST) - 1;
  out[o] = 0;
  return o;
}

// Yazılı soru: bu modelde yazıyla kullanıcı mesajı yollanmaz; o anki bağlama bir not eklenir.
size_t buildText(char *out, size_t cap, const char *text, int part) {
  if (part != 0) return 0;
  static char content[360];
  snprintf(content, sizeof(content), "Kullanıcı şu soruyu yazıyla gönderdi, şimdi sesli cevap ver: %s", text);
  return appendEvent(out, cap, "session.instructions.append", "soru", nullptr, content);
}

// Bu modelde "cevabı iptal et" komutu yok; araya giren bir not, süren konuşmayı kesebiliyor.
size_t buildInterrupt(char *out, size_t cap, int part, uint32_t) {
  if (part != 0) return 0;
  return appendEvent(out, cap, "session.instructions.append", "kes", nullptr,
                     "Kullanıcı az önce düğmeye basıp sözünü kesti. Yarım kalan cevabı bırak, sus ve kullanıcının ne diyeceğini bekle.");
}

// Aracın sonucu: function_call_output öğesi. Ardından buildContinue (response.create) gelmeli;
// yalnızca öğeyi eklemek arka modeli devam ettirmez (devretme kılavuzu).
size_t buildToolResult(char *out, size_t cap, const char *callId, const char *output) {
  JsonDocument d;
  char id[24];
  snprintf(id, sizeof(id), "arac_%u", (unsigned)++eventSeq);
  d["type"] = "response.item.create";
  d["event_id"] = id;
  JsonObject it = d["item"].to<JsonObject>();
  it["type"] = "function_call_output";
  it["call_id"] = callId;
  it["output"] = output;
  if (measureJson(d) + 1 > cap) return 0;
  return serializeJson(d, out, cap);
}

size_t buildContinue(char *out, size_t cap) {
  char m[80];
  int n = snprintf(m, sizeof(m), "{\"type\":\"response.create\",\"event_id\":\"devam_%u\"}", (unsigned)++eventSeq);
  if (n <= 0 || (size_t)n + 1 > cap) return 0;
  memcpy(out, m, (size_t)n + 1);
  return (size_t)n;
}

// Cihazdan modele not (zamanlayıcı çaldı gibi): oturum geneline talimat olarak eklenir, model söyler.
size_t buildNote(char *out, size_t cap, const char *text) {
  char id[24];
  snprintf(id, sizeof(id), "not_%u", (unsigned)++eventSeq);
  return appendEvent(out, cap, "session.instructions.append", id, nullptr, text);
}

size_t buildClose(char *out, size_t cap) {
  static const char M[] = "{\"type\":\"session.close\",\"event_id\":\"kapat\"}";
  if (sizeof(M) > cap) return 0;
  memcpy(out, M, sizeof(M));
  return sizeof(M) - 1;
}

size_t buildPending(char *out, size_t cap) {
  if (!pendingDelegation[0]) return 0;
  size_t n = appendEvent(out, cap, "session.commentary.append", "devir", pendingDelegation,
                         "Bu cihazda arama ya da başka bir araç yok. Kendi bilginle kısaca cevap ver; bilmiyorsan bilmediğini söyle.");
  pendingDelegation[0] = 0;
  return n;
}

bool handle(const char *msg, size_t len, ModelSink &sink) {
  size_t tyLen = 0;
  const char *ty = topString(msg, len, "type", &tyLen);

  // Ses: büyük ve sık; ağaç kurmadan çöz
  if (ty && typeIs(ty, tyLen, "session.output_audio.delta")) {
    size_t dLen = 0;
    const char *d = topString(msg, len, "delta", &dLen);
    size_t got = d ? decodeAudio(d, dLen, sink) : 0;
    if (logFirstEvents && !audioSeen) {
      audioSeen = true;
      char b[200];
      size_t head = d ? (size_t)(d - msg) : len;           // mesajın sesten önceki kısmı
      if (head > 70) head = 70;
      int n = snprintf(b, sizeof(b), "ilk ses parçası %u bayt (%u ms). Mesajın başı: %.*s", (unsigned)got,
                       (unsigned)(got * 1000 / 48000), (int)head, msg);
      sink.onOther("olay", b, n > 0 ? (size_t)n : 0);
    }
    return true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg, len, DeserializationOption::Filter(filterDoc()),
                                             DeserializationOption::NestingLimit(24));
  if (err) {
    sink.onOther("json-hata", msg, len);
    return false;
  }
  const char *type = doc["type"] | "";
  size_t tl = strlen(type);
  if (logFirstEvents && (typeStarts(type, tl, "session.") || typeStarts(type, tl, "response.") || typeIs(type, tl, "error")) &&
      firstSight(type, doc["event"]["type"] | ""))
    sink.onOther("olay", msg, len);

  if (typeIs(type, tl, "session.started")) {
    // Oturumun kendiliğinden kapanacağı an (belgede süre sınırı yazmıyor; buradan öğrenilir)
    long long exp = doc["session"]["expires_at"] | 0LL;
    long long nowS = (long long)time(nullptr);
    if (exp > 0 && nowS > 1700000000LL && exp > nowS) {
      char b[96];
      int n = snprintf(b, sizeof(b), "sunucu bu oturumu en geç %lld dk sonra kendiliğinden kapatır", (exp - nowS + 30) / 60);
      sink.onOther("oturum süresi", b, n > 0 ? (size_t)n : 0);
    }
    sink.onSetupComplete();
    return true;
  }

  if (typeIs(type, tl, "session.input_transcript.delta")) {
    const char *t = doc["delta"] | "";
    long e = doc["end_ms"] | -1L;
    if (e >= 0) userEndMs = e;
    lastWasUser = true;
    if (*t) sink.onText(true, t);
    return true;
  }
  if (typeIs(type, tl, "session.output_transcript.delta")) {
    const char *t = doc["delta"] | "";
    long st = doc["start_ms"] | -1L;
    if (lastWasUser && st >= 0 && userEndMs >= 0) {
      char b[120];
      long d = st - userEndMs;
      int n = d >= 0 ? snprintf(b, sizeof(b), "model, sözünün bitiminden %ld ms sonra konuşmaya başladı (sunucunun saati)", d)
                     : snprintf(b, sizeof(b), "model, sözün bitmeden %ld ms önce konuşmaya başladı (sunucunun saati)", -d);
      sink.onOther("söz sırası", b, n > 0 ? (size_t)n : 0);
    }
    lastWasUser = false;
    if (*t) sink.onText(false, t);
    return true;
  }

  if (typeIs(type, tl, "error")) {
    char b[300];
    const char *code = doc["error"]["code"] | "";
    const char *m = doc["error"]["message"] | "(mesaj yok)";
    const char *param = doc["error"]["param"] | "";
    int n = *code ? snprintf(b, sizeof(b), "%s: %s", code, m) : snprintf(b, sizeof(b), "%s", m);
    if (*param && n > 0 && (size_t)n < sizeof(b) && !strstr(b, param)) snprintf(b + n, sizeof(b) - (size_t)n, " (%s)", param);
    sink.onError(b);
    return true;
  }

  if (typeIs(type, tl, "session.closed")) {
    sink.onSessionEnd(doc["reason"] | "sebep yazmıyor", doc["usage"]["seconds"] | 0.0f);
    return true;
  }

  if (typeIs(type, tl, "session.delegation.created")) {
    const char *id = doc["delegation"]["id"] | "";
    const char *target = doc["delegation"]["target"] | "";
    if (!strcmp(target, "client")) {
      // Arka model yok: iş cihaza devredildi. Cevapsız kalırsa model bekler; "araç yok" denir.
      strncpy(pendingDelegation, id, sizeof(pendingDelegation) - 1);
      pendingDelegation[sizeof(pendingDelegation) - 1] = 0;
      static const char T[] = "model bir iş devretmek istedi; bu ayarda arka model yok, kendi bilgisiyle cevaplaması istendi";
      sink.onOther("devir", T, sizeof(T) - 1);
    } else if (!backendBusy) {
      backendBusy = true;
      static const char T[] = "soru arka modele devredildi (arama / düşünme)";
      sink.onOther("arka model", T, sizeof(T) - 1);
    }
    return true;
  }

  if (typeIs(type, tl, "response.event")) {
    const char *et = doc["event"]["type"] | "";
    if (!strcmp(et, "response.output_item.done")) {
      JsonObjectConst it = doc["event"]["item"].as<JsonObjectConst>();
      const char *itType = it["type"] | "";
      const char *callId = it["call_id"] | "";
      if (!strcmp(itType, "function_call") && *callId && !seenCall(callId))
        sink.onToolCall(callId, it["name"] | "", it["arguments"] | "{}");
      return true;
    }
    if (!strcmp(et, "response.completed")) {
      if (backendBusy) {
        backendBusy = false;
        static const char T[] = "cevabını verdi";
        sink.onOther("arka model", T, sizeof(T) - 1);
      }
    } else if (!strcmp(et, "response.failed") || !strcmp(et, "response.incomplete") || !strcmp(et, "error")) {
      backendBusy = false;
      char b[260];
      const char *m = doc["event"]["response"]["error"]["message"] | (doc["event"]["message"] | "(sebep yazmıyor)");
      snprintf(b, sizeof(b), "arka model cevap veremedi (%s): %s", et, m);
      sink.onError(b);
    }
    return true;
  }

  // Bildiğimiz ama işimize yaramayan olay aileleri: sessizce geç
  if (typeStarts(type, tl, "session.") || typeStarts(type, tl, "response.")) return true;

  sink.onOther("bilinmeyen", msg, len);
  return true;
}

// ---------------------------------------------------------------- sağlayıcı tablosu
static size_t pPath(char *out, size_t cap, const char *, const char *) {
  int n = snprintf(out, cap, "/v1/live/sessions");        // model adı adreste değil, session.start içinde gider
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
static size_t pHeaders(char *out, size_t cap, const char *key) {
  int n = snprintf(out, cap, "Authorization: Bearer %s\r\n", key);
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
static size_t pMicPause(char *, size_t) { return 0; }     // mikrofon kapanınca sessizlik akar; ayrı mesaj gerekmez

const Provider *provider() {
  static const Provider P = {
    "gptlive", "GPT-Live", 3, false, true, true,
    pPath, pHeaders, reset, buildSetup, buildAudio, buildText, pMicPause, buildInterrupt, handle, buildClose, buildPending,
    buildToolResult, buildContinue, buildNote,
  };
  return &P;
}

}  // namespace gptlive
