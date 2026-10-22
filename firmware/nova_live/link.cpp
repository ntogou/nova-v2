#include "link.h"
#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "config.h"
#include "secrets.h"
#include "audio.h"
#include "ui.h"
#include "gptlive.h"
#include "netlog.h"

// Anahtar tanımlanmamışsa bağlanırken açık bir hata verilir.
#ifndef OPENAI_API_KEY
#define OPENAI_API_KEY ""
#endif

namespace {

const size_t SYS_CAP = 32768;          // kişilik (~8,5 KB) + kişisel profil + hafıza (en çok ~11,5 KB) + gündem (~1,7 KB) + hava + zamanlayıcılar + tarih, PSRAM'de
const size_t BACKEND_CAP = 3072;       // arka modelin talimatı (tarih/saat + araç kuralları), PSRAM'de
const int TOOLQ_N = 4;                 // aynı anda bekleyebilecek araç çağrısı
const size_t TOOL_OUT_CAP = 1024;      // bir aracın modele dönen sonucu
const size_t HANDLE_CAP = 2048;
const uint32_t HANDLE_MAX_AGE_MS = 110UL * 60UL * 1000UL;   // devam anahtarı 2 saat geçerli

void logLine(const char *fmt, ...) {
  char b[512];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  Log.println(b);
}

bool containsNoCase(const char *hay, const char *needle) {
  size_t n = strlen(needle);
  for (; *hay; hay++) {
    size_t i = 0;
    while (i < n && hay[i] && tolower((unsigned char)hay[i]) == needle[i]) i++;
    if (i == n) return true;
  }
  return false;
}

// Yeniden denemenin işe yaramayacağı retler: anahtar, yetki, kota.
bool isFatalReason(const char *r) {
  static const char *const K[] = {"api key", "api_key", "permission", "quota", "billing",
                                  "unauthenticated", "unregistered", "model_not_found",
                                  "do not have access", "does not have access"};
  for (size_t i = 0; i < sizeof(K) / sizeof(K[0]); i++) if (containsNoCase(r, K[i])) return true;
  return false;
}

struct Endpoint { const char *host; uint16_t port; const char *model; const char *voice; const char *key; const char *keyName; };

struct Link : ModelSink {
  WsTransport *t = nullptr;
  LinkHooks h{};
  WsClient ws;
  // Sağlayıcı: GPT-Live. Akış proto.h'deki Provider arayüzü üzerinden yürür; sırayla konuşan sağlayıcılar
  // (2.4.3'e kadar Gemini Live ve GPT Realtime, şimdi firmware/arsiv/saglayicilar) aynı arayüzü uygular.
  const Provider *P = gptlive::provider();
  // Mikrofonlar çalışıyor mu (ana döngü bildirir). Çalışmıyorsa çerçeveler modele gitmez:
  // bozuk veri hem boşuna akar hem de sunucunun konuşma algısını yanıltabilir.
  volatile bool micOk = true;
  bool micWarned = false;
  bool setupFailed = false;             // kurulum sırasında sunucudan hata mesajı geldi
  char setupErr[300] = "";
  int unknownLogged = 0;

  const char *voiceOv = nullptr;       // kullanıcının seçtiği ses tonu (yoksa config.h'deki GPTLIVE_VOICE)
  const char *voiceName() const { return voiceOv && *voiceOv ? voiceOv : GPTLIVE_VOICE; }
  Endpoint endpoint() const {
    return Endpoint{OPENAI_HOST, OPENAI_PORT, GPTLIVE_MODEL, voiceName(), OPENAI_API_KEY, "OPENAI_API_KEY"};
  }

  // Sağlayıcı etiketiyle kayıt satırı: "[gptlive] ..."
  void tagLine(const char *fmt, ...) {
    char b[560];
    int n = snprintf(b, sizeof(b), "[%s] ", P->name);
    va_list a;
    va_start(a, fmt);
    vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, a);
    va_end(a);
    Log.println(b);
  }
  char *sysText = nullptr;
  char *backendBuf = nullptr;
  char *handle = nullptr;
  uint32_t handleAt = 0;

  volatile LinkState state = LINK_IDLE;
  volatile bool netUp = false, verbose = true;
  volatile bool reqWake = false, reqSleep = false, reqInterrupt = false, reqSay = false;
  char sayBuf[200] = "";
  char pendingText[200] = "";

  // bağlanma
  enum : uint8_t { PH_OPEN, PH_SETUP };
  uint8_t phase = PH_OPEN;
  int level = 0, attempts = 0;
  bool usedHandle = false, setupOk = false, resuming = false;
  uint32_t setupDeadline = 0, retryAt = 0;

  // konuşma
  bool genDone = true, turnDone = true, dropTurn = false, goAwayPending = false;
  uint32_t micOpenAt = 0, idleDeadline = 0, quietDeadline = 0, armedAt = 0;
  uint32_t lastAudioAt = 0, lastTxAt = 0, thinkSince = 0, dropSince = 0, stableSince = 0;
  // gecikme ölçümü: yerelde son ses, sunucunun "söz bitti" demesi, yazılı sorunun gidişi
  uint32_t localSpeechAt = 0, vadEndAt = 0, askedAt = 0;
  // Kısa kapanış (2.7.1): Nova'nın cevabı soruyla bitmediyse ve kullanıcı birkaç saniye konuşmazsa oturum kapanır.
  // shortDeadline 0 = kapalı; localLoudAt: mikrofonda son yüksek ses (konuşmaya başlamış olabilir, yazısı gelmedi)
  uint32_t shortDeadline = 0, localLoudAt = 0;
  // Sunucunun zaman damgasıyla ölçüm: bu bağlantıda gönderilen ses çerçevesi sayısı ve
  // "söz bitti" haberi geldiğinde sözün bitiminden bu yana geçen süre (-1: bilinmiyor)
  uint32_t sessFrames = 0;
  int32_t vadLagMs = -1;
  uint32_t turnAudioBytes = 0;
  int reconnects = 0;

  bool voiceOff = false;                // sunucu ses adını reddettiyse varsayılan sesle devam
  bool heardSpeech = false, blocked = false;
  int quietFrames = 0;
  // Sunucu konuşmayı algıladığında haber verir (Gemini: voiceActivity, OpenAI: speech_started /
  // speech_stopped). Bir kez görüldüyse "dinliyor /
  // düşünüyor" ve uyku sayacı yalnızca ona bakar; yerel tahmin (tepe ölçümü) devre dışı kalır.
  bool vadSeen = false, vadActive = false;

  // Nova'nın yazısı sesle birlikte gelir ama ses gerçek zamandan hızlı iner;
  // ekrandaki yazı çalınan sese göre açılsın diye parçalar sıraya konur.
  struct Caption { uint32_t atMs; char text[92]; };
  static const int CAP_N = 96;
  Caption *caps = nullptr;             // CAP_N kayıt, PSRAM'de (linkBegin)
  int capHead = 0, capTail = 0;

  // sayaçlar
  uint32_t turns = 0, sentFrames = 0, backpressure = 0, emptyMsgs = 0;
  long tokens = 0;

  // Serial'e tam cümle yazmak için biriktirilen yazı
  char (*lineBuf)[640] = nullptr;      // [2]: 0 = nova, 1 = sen; iki konuşanın yazısı ayrı birikir (PSRAM'de)
  uint32_t lineAt[2] = {0, 0};         // o konuşandan son yazı parçasının geldiği an

  // ---- Aynı anda dinleyip konuşan sağlayıcı (GPT-Live) için
  uint32_t tailSilentBytes = 0;        // son sesli parçadan sonra hoparlöre yazılan sessizlik
  uint32_t rxLoudAt = 0;               // son sesli parçanın geldiği an (atılan tur dahil)
  // Ses akışının kendisi (sessiz parçalar dahil): GPT-Live 100 ms'de bir parça yollar. Hoparlör boşaldığında
  // akışta uzun bir boşluk varsa ses ağdan geç gelmiştir; yoksa model duraksamıştır (sessizlik yollamıştır).
  uint32_t rxPrevAt = 0, turnMaxGap = 0;
  uint32_t silentFrames = 0;           // mikrofon kapalıyken gönderilen sessizlik çerçeveleri
  uint32_t speakSilentSkip = 0;        // konuşma sırasında seyreltilen sıfır çerçevesi sayacı
  uint32_t speakUnderruns = 0;         // konuşma başlarken hoparlörün "ses yetişmedi" sayacı
  uint32_t netLate = 0, modelPauses = 0;   // bu oturumda: ağdan geç gelen ses / modelin duraksaması
  uint32_t txPeakMax = 0, txLoudFrames = 0;  // son durum satırından beri modele giden en yüksek tepe, yüksek çerçeve sayısı
  uint32_t pingAt = 0;
  // Ön tampon bu oturumda uyarlanır: cevabın ortasında ses ağdan geç gelip hoparlör beklediyse büyütülür
  int prebufMs = GPTLIVE_PREBUF_MS;
  uint32_t underrunSeen = 0;
  uint32_t speakAt = 0;                // bu cevabın ilk sesinin geldiği an
  uint32_t droppedSilentBytes = 0;     // model susarken gelen ve çalınmayan sessizlik
  // Gelen sesin "konuşma mı sessizlik mi" eşiği ve son birkaç saniyenin en yüksek seviyesi
  // (1,5 sn'lik iki pencere). Eşik, sunucu susarken hışırtı yolluyorsa çalışırken yükselir.
  uint32_t outSilenceRms = GPTLIVE_OUT_SILENCE_RMS;
  uint32_t winMax = 0, winPrevMax = 0, winAt = 0, silentMaxRms = 0;
  // Cevap sürüyor sayılıyor ama hoparlör sustu (cümle arası duraklama ya da cevabın sonu): mikrofon
  // açıktır. Ses yeniden gelirse kapanır; GPTLIVE_ANSWER_END_MS boyunca gelmezse cevap biter.
  bool spkQuiet = false;
  bool userCutPause = false;           // duraklamada kullanıcı konuştu: cevap bitmiş sayılacak
  bool userTurnOpen = false;           // kullanıcıdan yazı geldi; durunca ekran "düşünüyor"a geçer
  bool inUserBracket = false;          // döküm modelinin "[sniff]", "[breath]" etiketi sürüyor mu
  uint32_t backendAt = 0;              // arka modele sorunun devredildiği an (süresini yazmak için)

  // ---- Cihaz araçları: arka modelin çağrıları sırayla yürütülür (mesaj işlenirken değil, döngüde)
  struct ToolCall { char id[72]; char name[40]; char args[768]; };
  ToolCall *toolQ = nullptr;            // TOOLQ_N kayıt, PSRAM'de
  char *toolOut = nullptr;              // TOOL_OUT_CAP bayt, PSRAM'de
  int toolN = 0;
  uint32_t toolCalls = 0;
  // ---- Cihazdan Nova'ya not (zamanlayıcı çaldı gibi)
  volatile bool reqNote = false;
  char noteBuf[240] = "";
  char pendingNote[240] = "";

  // ---- Uyandırma sözü: motor yalnızca uykudayken kurulu
  // ---- Araya girme (GPT-Live + yankı silme)
  volatile bool bargeIn = false;
  bool reqVoiceCut = false;            // Nova konuşurken kullanıcının sözü yazıya döküldü
  bool preSpeechUserText = false;      // konuşma başlamadan önce başlayan kullanıcı sözünün devamı mı
  uint32_t voiceCuts = 0;
  bool duplexNow() const { return bargeIn && P->fullDuplex && h.echoActive && h.echoActive(); }

  volatile bool wakeEnabled = true, reqWakeRearm = false, hold = false;
  volatile bool netBusy = false;        // cihaz başka bir şifreli bağlantı kuruyor (hava durumu): motor iç RAM'i boşaltsın
  bool wakeArmed = false;
  uint32_t wakeArmAt = 0, wakeHits = 0;

  // ---- Sesle oturum kapatma (bütün sağlayıcılar): kullanıcının son sözlerinin sadeleştirilmiş hali
  char byeTail[112] = "";
  uint32_t byeFedAt = 0, byeAt = 0;
  bool byePending = false;
  bool closing = false;                // kapanış onayı bekleniyor: gelen ses ve yazı yok sayılır
  // Ücret bağlı kalınan süreyle işler: oturum süresi ve açılıştan beri toplam
  uint32_t sessStartAt = 0;
  bool sessOpen = false;
  bool sessUserSpoke = false;          // bu oturumda kullanıcıdan söz ya da yazılı soru geldi
  float bootSeconds = 0;
  // Oturum kapanınca geçmiş sunucuda kalmaz; son satırlar burada tutulur ve yeni oturuma yazı olarak verilir.
  struct Hist { uint32_t at; bool user; char text[GPTLIVE_HISTORY_TEXT]; };
  Hist *hist = nullptr;
  int histN = 0;
  bool historyOff = false, usedHistory = false;
  int usedHistoryN = 0;

  uint32_t now() { return h.nowMs(); }
  static bool after(uint32_t nowMs, uint32_t deadline) { return (int32_t)(nowMs - deadline) >= 0; }
  // t anından bu yana geçen süre. t "şimdi"den sonraysa (aynı turda güncellendiyse) 0 döner.
  static uint32_t since(uint32_t nowMs, uint32_t t) { int32_t d = (int32_t)(nowMs - t); return d < 0 ? 0 : (uint32_t)d; }

  // ---------------------------------------------------------------- yazı
  uint32_t turnAudioMs() const { return (uint32_t)((uint64_t)turnAudioBytes * 1000 / (2 * SPK_RATE)); }
  uint32_t playedMs() { uint32_t tot = turnAudioMs(), buf = spkBufferedMs(); return tot > buf ? tot - buf : 0; }

  void queueCaption(const char *text) {
    size_t len = strlen(text);
    while (len) {
      int next = (capHead + 1) % CAP_N;
      if (next == capTail) { showCaptions(true); next = (capHead + 1) % CAP_N; }   // kuyruk doldu: bekleyenleri göster
      Caption &c = caps[capHead];
      c.atMs = turnAudioMs();
      size_t take = len < sizeof(c.text) - 1 ? len : sizeof(c.text) - 1;
      while (take < len && take > 1 && ((uint8_t)text[take] & 0xC0) == 0x80) take--;   // UTF-8 harfi ortadan bölme
      memcpy(c.text, text, take);
      c.text[take] = 0;
      capHead = next;
      text += take;
      len -= take;
    }
  }
  void showCaptions(bool all) {
    uint32_t p = all ? 0xFFFFFFFFu : playedMs();
    while (capTail != capHead && caps[capTail].atMs <= p) {
      uiText("nova", caps[capTail].text, false);
      capTail = (capTail + 1) % CAP_N;
    }
  }
  void dropCaptions() { capTail = capHead; }

  // ---------------------------------------------------------------- sesle oturum kapatma
  // Bir UTF-8 harfini karşılaştırma için sadeleştirir: küçük harf, Türkçe harfler yalın karşılığına
  // (ı İ -> i, ş -> s, ğ -> g, ü -> u, ö -> o, ç -> c); harf ve rakam dışındaki her şey boşluk.
  static char foldChar(const char *&p) {
    uint8_t c = (uint8_t)*p++;
    if (c < 0x80) {
      if (c >= 'A' && c <= 'Z') return (char)(c + 32);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return (char)c;
      return ' ';
    }
    uint8_t d = (uint8_t)*p;
    if ((c & 0xE0) == 0xC0 && (d & 0xC0) == 0x80) {
      p++;
      uint16_t cp = (uint16_t)(((c & 0x1F) << 6) | (d & 0x3F));
      switch (cp) {
        case 0xC7: case 0xE7: return 'c';
        case 0xD6: case 0xF6: return 'o';
        case 0xDC: case 0xFC: case 0xDB: case 0xFB: return 'u';
        case 0xC2: case 0xE2: return 'a';
        case 0xCE: case 0xEE: case 0x130: case 0x131: return 'i';
        case 0x11E: case 0x11F: return 'g';
        case 0x15E: case 0x15F: return 's';
      }
      return ' ';
    }
    while (((uint8_t)*p & 0xC0) == 0x80) p++;      // daha uzun diziler: atla
    return ' ';
  }
  // text içinde, kelime başında başlayan son phrase'i bulur
  static const char *findPhrase(const char *text, const char *phrase) {
    const char *hit = nullptr;
    size_t n = strlen(phrase);
    for (const char *q = text; (q = strstr(q, phrase)) != nullptr; q += n)
      if (q == text || q[-1] == ' ') hit = q;
    return hit;
  }
  static int countWords(const char *t) {
    int w = 0;
    bool in = false;
    for (; *t; t++) {
      if (*t != ' ' && !in) { in = true; w++; }
      else if (*t == ' ') in = false;
    }
    return w;
  }
  // Kullanıcının sözünün yeni parçası geldi: son sözlerin sadeleştirilmiş halini güncelle, veda var mı bak.
  void byeFeed(const char *text) {
    uint32_t n = now();
    size_t len = strlen(byeTail);
    if (len && since(n, byeFedAt) > 6000) { byeTail[0] = 0; len = 0; }      // eski söz: yeni cümle
    byeFedAt = n;
    for (const char *p = text; *p;) {
      char c = foldChar(p);
      if (c == ' ' && (len == 0 || byeTail[len - 1] == ' ')) continue;
      if (len >= sizeof(byeTail) - 1) { memmove(byeTail, byeTail + 40, len - 40 + 1); len -= 40; }
      byeTail[len++] = c;
      byeTail[len] = 0;
    }
    static const char *const CMD[] = {HANGUP_COMMANDS};
    static const char *const BYE[] = {HANGUP_FAREWELLS};
    const char *hit = nullptr;
    for (size_t i = 0; i < sizeof(CMD) / sizeof(CMD[0]) && !hit; i++)
      if (findPhrase(byeTail, CMD[i])) hit = CMD[i];
    for (size_t i = 0; i < sizeof(BYE) / sizeof(BYE[0]) && !hit; i++) {
      // Veda, kısa bir sözün sonunda olmalı: ardından en çok bir kelime ("görüşürüz nova")
      const char *q = findPhrase(byeTail, BYE[i]);
      if (!q) continue;
      const char *after = q + strlen(BYE[i]);
      if ((*after == 0 || *after == ' ') && countWords(after) <= 1 && countWords(byeTail) <= 7) hit = BYE[i];
    }
    if (hit) {
      byeAt = n;
      if (!byePending) {
        byePending = true;
        logLine("[bağ] veda algılandı (\"%s\"): Nova'nın cevabı bitince oturum kapanacak", hit);
      }
    } else if (byePending) {
      byePending = false;
      logLine("[bağ] veda sayılmadı: söz devam etti");
    }
  }
  void byeClose() {
    tagLine("vedalaşıldı, oturum kapatıldı. %s", wakeHint());
    toIdle();
  }
  const char *wakeHint() const {
    return wakeEnabled && h.wakeArm ? "Uyandırmak için \"Hey Nova\" de ya da BOOT'a bas." : "Uyandırmak için BOOT'a bas.";
  }
  // Nova vedalaştı mı: kısa bir cevap vedayı içeriyorsa ("Tabii, görüşürüz.") ya da cevap, uzunluğu ne olursa
  // olsun vedayla bitiyorsa ("Rica ederim, iyi uykular. Görüşürüz."; ardından en çok bir kelime: "görüşürüz Doğu").
  // 2.7.1: kullanıcı teşekkür edip konuyu kapatınca Nova kısa bir karşılık verip "Görüşürüz." diyor.
  static bool novaSaidBye(const char *text) {
    char f[200];
    size_t len = 0;
    bool kesildi = false;
    const char *p = text;
    while (*p) {
      char c = foldChar(p);
      if (c == ' ' && (len == 0 || f[len - 1] == ' ')) continue;
      if (len >= sizeof(f) - 1) { memmove(f, f + 60, len - 60); len -= 60; kesildi = true; }   // yalnızca son kısım
      f[len++] = c;
    }
    while (len && f[len - 1] == ' ') len--;
    f[len] = 0;
    if (!len) return false;
    bool kisa = !kesildi && countWords(f) <= NOVA_BYE_MAX_WORDS;
    static const char *const BYE[] = {NOVA_FAREWELLS};
    for (size_t i = 0; i < sizeof(BYE) / sizeof(BYE[0]); i++) {
      const char *q = findPhrase(f, BYE[i]);
      if (!q) continue;
      const char *after = q + strlen(BYE[i]);
      if (*after != 0 && *after != ' ') continue;
      if (kisa || countWords(after) <= 1) return true;
    }
    return false;
  }

  void flushLine(bool user) {
    char *l = lineBuf[user ? 1 : 0];
    if (!l[0]) return;
    logLine("%s %s", user ? "[sen] " : "[nova]", l);
    if (user) sessUserSpoke = true;
    histAdd(user, l);
    if (user && h.heard) h.heard(l);
    if (h.line) h.line(user, l);
    l[0] = 0;
  }

  // ---------------------------------------------------------------- konuşma geçmişi (GPT-Live)
  void histAdd(bool user, const char *text) {
    if (!hist || !P->fullDuplex || !*text) return;
    uint32_t n = now();
    const size_t cap = sizeof(hist[0].text);
    size_t add = strlen(text);
    if (histN && hist[histN - 1].user == user && since(n, hist[histN - 1].at) < 20000) {
      // Aynı konuşanın art arda gelen satırları tek kayıtta birleşir
      Hist &e = hist[histN - 1];
      size_t len = strlen(e.text);
      if (len + 1 + add < cap) {
        e.text[len] = ' ';
        memcpy(e.text + len + 1, text, add + 1);
        e.at = n;
        return;
      }
    }
    if (histN == GPTLIVE_HISTORY_LINES) {
      memmove(hist, hist + 1, (size_t)(GPTLIVE_HISTORY_LINES - 1) * sizeof(Hist));
      histN--;
    }
    Hist &e = hist[histN++];
    e.at = n;
    e.user = user;
    if (add >= cap) {
      add = cap - 1;
      while (add > 1 && ((uint8_t)text[add] & 0xC0) == 0x80) add--;      // UTF-8 harfi ortadan bölme
    }
    memcpy(e.text, text, add);
    e.text[add] = 0;
  }
  // Eskimiş satırları atar; kalanları ModelConfig'e verilecek biçimde dizer. Satır sayısını döner.
  int histCollect(ChatLine *out) {
    if (!hist) return 0;
    uint32_t n = now();
    int drop = 0;
    while (drop < histN && since(n, hist[drop].at) > GPTLIVE_HISTORY_MAX_AGE_MS) drop++;
    if (drop) {
      memmove(hist, hist + drop, (size_t)(histN - drop) * sizeof(Hist));
      histN -= drop;
    }
    for (int i = 0; i < histN; i++) { out[i].user = hist[i].user; out[i].text = hist[i].text; }
    return histN;
  }

  // ---------------------------------------------------------------- ücretlendirilen süre (GPT-Live)
  void accountSession(float serverSeconds, const char *how) {
    if (!sessOpen) return;
    sessOpen = false;
    float sec = serverSeconds > 0 ? serverSeconds : since(now(), sessStartAt) / 1000.0f;
    bootSeconds += sec;
    tagLine("oturum %s: %.0f sn bağlı kalındı%s. Açılıştan beri %.1f dk, ses ücreti yaklaşık %.2f $ (arka model ayrıca).",
            how, sec, serverSeconds > 0 ? "" : " (cihazın ölçümü)", bootSeconds / 60.0f,
            bootSeconds / 60.0f * GPTLIVE_USD_PER_MIN);
    if (h.usage) h.usage(sec, sessUserSpoke);
  }

  // Bağlantıyı kapatır. Sağlayıcı kapanış isteği tanımlıyorsa (GPT-Live) önce onu yollar ve
  // onayı kısa süre bekler: süre ücreti sunucuda kesinleşsin ve Serial'e yazılabilsin.
  void closeSession() {
    if (P->buildClose && ws.isOpen() && sessOpen) {
      size_t len = P->buildClose((char *)ws.txPayload(), ws.txPayloadCap());
      if (len && ws.send(WsClient::OP_TEXT, len)) {
        closing = true;
        uint32_t t0 = now();
        while (closing && ws.isOpen() && since(now(), t0) < 1500) {
          int r = ws.poll();
          if (r < 0) break;
          if (r == 1) { P->handle((const char *)ws.msg(), ws.msgLen(), *this); ws.consume(); }
          else if (h.idle) h.idle();
        }
        closing = false;
      }
    }
    ws.close();
    if (P->fullDuplex) accountSession(0, "kapandı (onay gelmedi)");
  }
  void flushLines() { flushLine(true); flushLine(false); }
  void addLine(bool user, const char *text) {
    // Yazılar sırayla gelen sağlayıcıda (Gemini) konuşan değişince öncekinin satırı basılır.
    // İç içe gelen sağlayıcıda (GPT) her satır kendi bitişinde basılır: kullanıcınınki
    // onUserTextDone ile, Nova'nınki cevap bitince.
    if (!P->userTextAsync) flushLine(!user);
    char *l = lineBuf[user ? 1 : 0];
    const size_t cap = sizeof(lineBuf[0]);
    if (!l[0]) while (*text == ' ') text++;         // parçalar boşlukla başlar: satır başında gereksiz
    size_t len = strlen(l), n = strlen(text);
    if (len + n >= cap - 1) { flushLine(user); len = 0; }
    if (n >= cap - 1) n = cap - 2;
    memcpy(l + len, text, n);
    l[len + n] = 0;
  }

  // ---------------------------------------------------------------- durum geçişleri
  void toIdle() {
    flushLines();
    dropCaptions();
    closeSession();
    state = LINK_IDLE;
    reqWake = false;
    reqSleep = false;
    reqInterrupt = false;
    dropTurn = false;
    goAwayPending = false;
    pendingText[0] = 0;
    pendingNote[0] = 0;
    toolN = 0;
    byePending = false;
    byeTail[0] = 0;
    spkQuiet = false;
    userTurnOpen = false;
    inUserBracket = false;
    backendAt = 0;
    reqVoiceCut = false;
    preSpeechUserText = false;
    if (h.echoCancel) h.echoCancel(false);   // yankı silme yalnızca oturumda (iç RAM uyandırma motoruna kalsın)
    spkSetBoostDb(SPK_BOOST_DB);          // açılış sesi ve testler varsayılan kazançla çalar
    wakeArmAt = now() + WAKE_HOLDOFF_MS;  // Nova'nın son sözünün yankısı sönsün
  }

  void fatal(const char *why) {
    tagLine("HATA: %s", why);
    char b[220];
    snprintf(b, sizeof(b), "Hata: %s", why);
    uiText("nova", b, true);
    spkFlush();
    toIdle();
  }

  void beginConnect(bool resume) {
    attempts = 0;
    phase = PH_OPEN;
    retryAt = now();
    resuming = resume;
    state = LINK_CONNECTING;
    if (!resume) { uiClearText(); reconnects = 0; micWarned = false; }
  }

  void sendPendingText() {
    if (!pendingText[0] || !ws.isOpen()) return;
    bool sent = false;
    for (int part = 0; part < 4; part++) {
      size_t n = P->buildText((char *)ws.txPayload(), ws.txPayloadCap(), pendingText, part);
      if (!n) break;
      if (!ws.send(WsClient::OP_TEXT, n)) { sent = false; break; }
      sent = true;
    }
    if (sent) {
      logLine("[sen]  (yazılı) %s", pendingText);
      sessUserSpoke = true;
      uiText("user", pendingText, true);
      lastTxAt = askedAt = now();
      armIdle(askedAt);
    }
    pendingText[0] = 0;
  }

  // Cihazdan not: model bunu kullanıcıya söyler (zamanlayıcı çaldı gibi)
  void sendPendingNote() {
    if (!pendingNote[0] || !ws.isOpen()) return;
    if (!P->buildNote) { pendingNote[0] = 0; return; }
    size_t len = P->buildNote((char *)ws.txPayload(), ws.txPayloadCap(), pendingNote);
    if (len && ws.send(WsClient::OP_TEXT, len)) {
      logLine("[bildirim] %s", pendingNote);
      sessUserSpoke = true;               // cihazın açtığı oturum: yanlış uyanma sayılmasın
      lastTxAt = now();
      armIdle(lastTxAt);
    }
    pendingNote[0] = 0;
  }

  void onToolCall(const char *callId, const char *name, const char *args) override {
    if (closing || !toolQ) return;
    if (toolN >= TOOLQ_N) { tagLine("araç çağrısı sığmadı, atlandı: %s", name); return; }
    ToolCall &c = toolQ[toolN++];
    strncpy(c.id, callId, sizeof(c.id) - 1);
    c.id[sizeof(c.id) - 1] = 0;
    strncpy(c.name, name, sizeof(c.name) - 1);
    c.name[sizeof(c.name) - 1] = 0;
    strncpy(c.args, args, sizeof(c.args) - 1);
    c.args[sizeof(c.args) - 1] = 0;
  }

  // Bekleyen araç çağrılarını yürütür, sonuçları yollar, sonra arka modeli bir kez devam ettirir.
  void runTools(uint32_t n) {
    char *result = toolOut;
    const size_t RES = TOOL_OUT_CAP;
    bool sentAny = false;
    for (int i = 0; i < toolN; i++) {
      ToolCall &c = toolQ[i];
      result[0] = 0;
      if (h.tool) h.tool(c.name, c.args, result, RES);
      else snprintf(result, RES, "Bu cihazda '%s' aracı yok.", c.name);
      if (!result[0]) snprintf(result, RES, "tamam");
      toolCalls++;
      logLine("[araç] %s %s -> %s", c.name, c.args, result);
      if (!P->buildToolResult || !ws.isOpen()) continue;
      size_t len = P->buildToolResult((char *)ws.txPayload(), ws.txPayloadCap(), c.id, result);
      if (len && ws.send(WsClient::OP_TEXT, len)) sentAny = true;
      else tagLine("araç sonucu gönderilemedi: %s", c.name);
    }
    toolN = 0;
    if (sentAny && P->buildContinue) {
      size_t len = P->buildContinue((char *)ws.txPayload(), ws.txPayloadCap());
      if (len) ws.send(WsClient::OP_TEXT, len);
    }
    lastTxAt = n;
    armIdle(n);                           // araç kullanımı da konuşmanın parçası: oturum açık kalsın
  }

  // Uyku sayaçlarını yeniden kurar. Yalnızca gerçek bir olayda çağrılır: oturumun açılması,
  // Nova'nın cevabının bitmesi, sunucunun konuşma algılaması, yazıya dökülen söz.
  void armIdle(uint32_t n) {
    armedAt = n;
    idleDeadline = n + LINK_IDLE_SLEEP_MS;
    quietDeadline = n + LINK_MAX_QUIET_MS;
    shortDeadline = 0;                    // gerçek bir olay (söz, araç, yeni cevap): kısa kapanış iptal
  }
  // Nova'nın cevabı soruyla mı bitti ("… kurayım mı?"): o zaman cevap beklenir, kısa kapanış olmaz
  static bool endsWithQuestion(const char *t) {
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '"' || t[n - 1] == '\'' || t[n - 1] == ')' || t[n - 1] == '\n')) n--;
    return n && t[n - 1] == '?';
  }
  // Cümle bitmiş mi (". ! …" ile). Ses takılıp cevap yarıda "bitti" sayılınca ("…kendi") kısa kapanış kurulmaz.
  static bool endsSentence(const char *t) {
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '"' || t[n - 1] == '\'' || t[n - 1] == ')' || t[n - 1] == '\n')) n--;
    if (!n) return false;
    if (t[n - 1] == '.' || t[n - 1] == '!') return true;
    return n >= 3 && (uint8_t)t[n - 3] == 0xE2 && (uint8_t)t[n - 2] == 0x80 && (uint8_t)t[n - 1] == 0xA6;   // …
  }

  // Yeni dinleme dönemi (oturum açıldı ya da Nova'nın sözü bitti)
  void enterListen(bool afterSpeech) {
    uint32_t n = now();
    state = LINK_LISTEN;
    // GPT-Live'da buraya gelindiğinde hoparlör zaten bir süredir susmuştur (bitiş, ses akışının
    // durmasından anlaşılır); ek bekleme kısa tutulur.
    micOpenAt = n + (afterSpeech ? (P->fullDuplex ? GPTLIVE_POST_SPEECH_GUARD_MS : LINK_POST_SPEECH_GUARD_MS) : 0);
    armIdle(n);
    heardSpeech = false;
    quietFrames = 0;
    vadActive = false;
    sendPendingText();
  }

  // "Düşünüyor"dan cevap gelmeden geri dönüş: yalnızca ekran değişir, sayaçlara dokunulmaz.
  void backToListen() {
    state = LINK_LISTEN;
    heardSpeech = false;
    quietFrames = 0;
  }

  void logFirstAudio(uint32_t n) {
    if (vadEndAt && vadLagMs >= 0) {
      uint32_t model = since(n, vadEndAt);
      char local[40] = "";
      if (localSpeechAt) snprintf(local, sizeof(local), " | yerel ölçüm ~%u ms", (unsigned)since(n, localSpeechAt));
      tagLine("<- ilk ses: sözün bitiminden %u ms sonra (%s'nin kararı %u ms + cevap %u ms)%s",
              (unsigned)(vadLagMs + model), P->label, (unsigned)vadLagMs, (unsigned)model, local);
    } else if (vadEndAt) {
      uint32_t model = since(n, vadEndAt);
      uint32_t total = localSpeechAt ? since(n, localSpeechAt) : 0;
      if (total > model)
        tagLine("<- ilk ses: sen sustuktan ~%u ms sonra (%s sözün bittiğine %u ms'de karar verdi, cevap ondan %u ms sonra geldi)",
                (unsigned)total, P->label, (unsigned)(total - model), (unsigned)model);
      else
        tagLine("<- ilk ses: %s \"söz bitti\" dedikten %u ms sonra", P->label, (unsigned)model);
    } else if (askedAt) {
      tagLine("<- ilk ses: yazılı sorudan %u ms sonra", (unsigned)since(n, askedAt));
    } else if (localSpeechAt) {
      tagLine("<- ilk ses: son sesten ~%u ms sonra", (unsigned)since(n, localSpeechAt));
    }
  }

  void enterSpeak() {
    uint32_t n = now();
    if (verbose) logFirstAudio(n);
    localSpeechAt = vadEndAt = askedAt = 0;
    vadLagMs = -1;
    vadActive = false;
    state = LINK_SPEAK;
    genDone = false;
    turnDone = false;
    turnAudioBytes = 0;
    tailSilentBytes = 0;
    spkQuiet = false;
    speakSilentSkip = 0;
    userCutPause = false;
    userTurnOpen = false;
    speakUnderruns = spkUnderrunCount();
    underrunSeen = speakUnderruns;
    turnMaxGap = 0;
    speakAt = n;
    lastAudioAt = n;
    preSpeechUserText = lineAt[1] && since(n, lineAt[1]) < 1500;
    if (P->fullDuplex && (!lineAt[1] || since(n, lineAt[1]) > 800)) flushLine(true);   // devam eden sözü ("Sel"+"am") ortadan bölme
    audioTakePlayStart();                 // önceki turdan kalan olayları temizle
    audioTakePlayEnd();
    // Mikrofon kapanıyor: sağlayıcı bunun için bir mesaj istiyorsa gönder (Gemini: audioStreamEnd)
    size_t len = P->buildMicPause((char *)ws.txPayload(), ws.txPayloadCap());
    if (len) ws.send(WsClient::OP_TEXT, len);
    lastTxAt = n;
  }

  void finishSpeak() {
    turns++;
    reconnects = 0;
    showCaptions(true);
    bool novaBye = !byePending && novaSaidBye(lineBuf[0]);
    // yazısı yoksa bilinmez; cümle yarım kaldıysa (ses takıldı) cevap sürüyor olabilir: ikisinde de kısa kapanış yok
    bool sorusuz = lineBuf[0][0] && !endsWithQuestion(lineBuf[0]) && endsSentence(lineBuf[0]);
    if (!P->userTextAsync) flushLine(true);       // GPT'de kullanıcının satırı kendi yazısı bitince basılır
    flushLine(false);
    if (verbose) {
      // Ses gerçek zamanlı akan sağlayıcıda hoparlörün boşta kaldığı anlar sayılır: cümle arasındaki
      // doğal duraklama da, ağ gecikmesi de buraya düşer (ayırt edilemez). Kısa cevapta sayı yüksekse
      // ses takılıyordur: GPTLIVE_PREBUF_MS büyütülür.
      uint32_t gaps = spkUnderrunCount() - speakUnderruns;
      if (P->fullDuplex)
        // "X sn'de geldi": sesle aynıysa sunucu gerçek zamanlı yolluyor, çok kısaysa önden yığıyor
        logLine("[nova]  cevap bitti: %.1f sn ses (%.1f sn'de geldi), akışta %u duraklama, en uzun ağ boşluğu %u ms",
                turnAudioBytes / (2.0f * SPK_RATE), since(lastAudioAt, speakAt) / 1000.0f, (unsigned)(gaps > 1 ? gaps - 1 : 0),
                (unsigned)turnMaxGap);
      else
        logLine("[nova]  cevap bitti: %.1f sn ses", turnAudioBytes / (2.0f * SPK_RATE));
    }
    bool wasQuiet = spkQuiet;
    spkQuiet = false;
    userCutPause = false;
    if (byePending) { byeClose(); return; }       // kullanıcı vedalaşmıştı: Nova'nın cevabı da bitti
    if (novaBye) {                                // kullanıcının sözü listede yoktu ama Nova vedalaştı
      logLine("[bağ] Nova vedalaştı (\"görüşürüz\"): oturum kapanıyor");
      byeClose();
      return;
    }
    byeTail[0] = 0;                               // yeni söz, yeni cümle
    // GPT-Live'da buraya gelindiğinde mikrofon zaten açıktır (hoparlör susalı beri): yeniden bekletilmez.
    enterListen(!wasQuiet);
    if (LINK_SHORT_CLOSE_MS > 0 && P->fullDuplex && sorusuz) shortDeadline = now() + LINK_SHORT_CLOSE_MS;
  }

  // Hoparlör sustu: mikrofonu aç ama cevabı hemen bitmiş sayma (GPT-Live cümle aralarında duraklar).
  void speakerQuiet(uint32_t n) {
    spkQuiet = true;
    micOpenAt = n + GPTLIVE_POST_SPEECH_GUARD_MS;
  }

  // ---------------------------------------------------------------- modelden gelenler
  void onSetupComplete() override {
    setupOk = true;
    if (verbose) tagLine("<- setupComplete");
  }

  void onAudio(const uint8_t *pcm, size_t bytes) override {
    if (closing || state == LINK_IDLE || state == LINK_CONNECTING) return;
    if (P->fullDuplex) {
      // Bu sağlayıcı cevabın bittiğini bildirmez ve akışta sessizlik de taşıyabilir: parçanın
      // seviyesine (RMS) bakılır. Sessiz parça konuşmayı başlatmaz, sürdürmez.
      uint64_t sq = 0;
      for (size_t i = 0; i + 1 < bytes; i += 2) {
        int32_t v = (int16_t)(pcm[i] | (pcm[i + 1] << 8));
        sq += (uint64_t)(v * v);
      }
      uint32_t rms = bytes >= 2 ? (uint32_t)sqrtf((float)(sq / (bytes / 2))) : 0;
      uint32_t t = now();
      uint32_t rxGap = rxPrevAt ? since(t, rxPrevAt) : 0;
      rxPrevAt = t;
      if (state == LINK_SPEAK && rxGap > turnMaxGap) turnMaxGap = rxGap;
      if (since(t, winAt) > 1500) { winPrevMax = winMax; winMax = 0; winAt = t; }
      if (rms > winMax) winMax = rms;
      if (rms < outSilenceRms) {
        if (rms > silentMaxRms) silentMaxRms = rms;
        if (dropTurn || state != LINK_SPEAK || genDone) { droppedSilentBytes += (uint32_t)bytes; return; }
        spkWrite(pcm, bytes);             // konuşmanın içindeki duraklama: ritim bozulmasın diye çalınır
        turnAudioBytes += (uint32_t)bytes;
        tailSilentBytes += (uint32_t)bytes;
        return;
      }
      rxLoudAt = now();
      if (dropTurn) return;
      // Cevap sürerken hoparlörün tamponu boşalmış ve ses şimdi geliyor. İki sebep ayrılır:
      //  - akışta uzun boşluk var: ses ağdan geç geldi (sunucu ya da internet yolu). Bu oturumun kalanında
      //    daha çok biriktirilir; takılma azalır, ilk ses biraz gecikir.
      //  - akış sürüyordu (sessiz parçalar geliyordu): model cümlenin ortasında duraksadı. Ön tampon büyütülmez;
      //    büyütmek takılmayı çözmez, her cevabın başını geciktirir (2.8.0'da öyle oluyordu).
      uint32_t ur = spkUnderrunCount();
      if (state == LINK_SPEAK && ur != underrunSeen) {
        underrunSeen = ur;
        if (rxGap >= GPTLIVE_NET_GAP_MS) {
          netLate++;
          if (prebufMs < GPTLIVE_PREBUF_MAX_MS) {
            prebufMs = prebufMs + 150 > GPTLIVE_PREBUF_MAX_MS ? GPTLIVE_PREBUF_MAX_MS : prebufMs + 150;
            spkSetPrebufMs(prebufMs);
          }
          logLine("[ses] ses geç geldi (akışta %u ms boşluk, son ping %u ms), hoparlör bekledi; bu oturumda ön tampon %d ms",
                  (unsigned)rxGap, (unsigned)ws.rttLast, prebufMs);
        } else {
          modelPauses++;
          if (verbose) logLine("[ses] Nova cümlenin ortasında duraksadı (akış sürüyordu, ağ değil); ön tampon %d ms kaldı", prebufMs);
        }
      }
      if (state == LINK_SPEAK && genDone) genDone = turnDone = false;      // bitti sanılmıştı, konuşma sürüyor
      spkQuiet = false;                                                     // duraklama bitti: mikrofon kapanır
      userCutPause = false;
      tailSilentBytes = 0;
    }
    if (dropTurn) return;
    if (state != LINK_SPEAK) enterSpeak();
    spkWrite(pcm, bytes);
    lastAudioAt = now();
    turnAudioBytes += (uint32_t)bytes;
  }

  void onText(bool fromUser, const char *text) override {
    if (closing) return;
    uint32_t n = now();
    uint32_t prevUserAt = lineAt[1];
    if (fromUser) {
      // Döküm modelinin ürettiği "[sniff]", "[breath]", "[laugh" gibi ses etiketlerini ayıkla;
      // yalnızca etiket ya da noktalama (".") geldiyse konuşma sayma (Nova'nın sözünü kesmesin,
      // ekranı boşuna "düşünüyor"da bekletmesin — 2.8.4 kaydı satır 228-238).
      char clean[256];
      size_t co = 0;
      bool hasWord = false;
      for (const char *p = text; *p && co + 1 < sizeof(clean); p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '[') { inUserBracket = true; continue; }
        if (inUserBracket) {
          if (c == ']' || c == ' ' || c == '\n') inUserBracket = false;
          continue;
        }
        if (c == ']') continue;
        clean[co++] = (char)c;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 0xC3 && c <= 0xC5))
          hasWord = true;
      }
      clean[co] = 0;
      if (!hasWord) return;
      lineAt[1] = n;
      if (P->fullDuplex) {
        // Duraklamada kullanıcı konuştu: Nova'nın cevabı burada bitmiş sayılır (ana döngü bitirir;
        // buradan oturum kapatılamaz, sunucu mesajı işlenirken çağrılıyoruz).
        if (state == LINK_SPEAK && spkQuiet) userCutPause = true;
        // Cevabın ilk anlarında gelen yazı ya da konuşma başlamadan önceki sözün kesintisiz devamı
        // ("Kestin dimi sen az" + " önce") önceki sözün geç gelen parçasıdır: Nova'yı kesmez.
        bool lateTail = state == LINK_SPEAK && (since(n, speakAt) <= GPTLIVE_BARGE_GUARD_MS ||
                        (preSpeechUserText && prevUserAt && since(n, prevUserAt) < 1200));
        if (state != LINK_SPEAK || lateTail) preSpeechUserText = true;
        else preSpeechUserText = false;
        // Araya girme: Nova konuşurken kullanıcının yeni sözü geldi (yankı silinmiş mikrofondan)
        if (state == LINK_SPEAK && !spkQuiet && duplexNow() && !lateTail) reqVoiceCut = true;
        if (state == LINK_THINK) state = LINK_LISTEN;
        if (state != LINK_SPEAK || !lateTail) userTurnOpen = true;
      }
      byeFeed(clean);
      armIdle(n);
      addLine(true, clean);
      uiText("user", clean, false);
      return;
    }
    lineAt[0] = n;
    if (dropTurn) return;                 // kesilen turun yazısı da atılır
    addLine(false, text);
    if (state == LINK_SPEAK) queueCaption(text);
    else uiText("nova", text, false);     // sesten önce gelen yazı beklemeden gösterilir
  }

  void onUserTextDone() override { flushLine(true); }

  void onGenerationComplete() override {
    if (verbose) tagLine("<- generationComplete");
    if (dropTurn) { dropTurn = false; genDone = true; return; }
    if (!genDone) {
      genDone = true;
      if (state == LINK_SPEAK) spkMarkEnd();
    }
  }

  void onTurnComplete() override {
    if (verbose) tagLine("<- turnComplete");
    turnDone = true;
    dropTurn = false;
    if (!genDone) {
      genDone = true;
      if (state == LINK_SPEAK) spkMarkEnd();
    }
    if (state == LINK_THINK) enterListen(false);
  }

  void onInterrupted() override {
    tagLine("<- interrupted (söz kesildi)");
    spkFlush();
    dropCaptions();
    genDone = turnDone = true;
    dropTurn = false;
    flushLines();
    if (state == LINK_SPEAK) enterListen(true);
  }

  void onVoiceActivity(bool start, int32_t offsetMs) override {
    uint32_t n = now();
    uint32_t sentMs = sessFrames * (uint32_t)(MIC_FRAME_SAMPLES * 1000 / MIC_RATE);
    if (verbose) {
      if (offsetMs >= 0)
        tagLine("<- voiceActivity %s (ses akışının %.2f. saniyesi; şu ana dek gönderilen %.2f sn)",
                start ? "başladı" : "bitti", offsetMs / 1000.0, sentMs / 1000.0);
      else
        tagLine("<- voiceActivity %s", start ? "başladı" : "bitti");
    }
    vadSeen = true;
    vadActive = start;
    armIdle(n);
    if (start) {
      vadEndAt = 0;
      vadLagMs = -1;
      if (state == LINK_THINK) state = LINK_LISTEN;      // düşünürken yeniden konuşmaya başladın
    } else {
      vadEndAt = n;
      // Sözün bitimi akışın offsetMs'inde; biz sentMs'e kadar yolladık. Fark, sunucunun
      // karar verip haber vermesinin ne kadar sürdüğü (akla yatkın değilse kullanılmaz).
      vadLagMs = (offsetMs >= 0 && sentMs >= (uint32_t)offsetMs && sentMs - (uint32_t)offsetMs < 30000)
                   ? (int32_t)(sentMs - (uint32_t)offsetMs) : -1;
      if (state == LINK_LISTEN) { state = LINK_THINK; thinkSince = n; }
    }
  }

  void onGoAway(int secondsLeft) override {
    tagLine("<- goAway: bağlantı %d sn sonra kapanacak, uygun anda yenilenecek", secondsLeft);
    goAwayPending = true;
  }

  void onResume(const char *hd, bool resumable) override {
    if (!resumable || !*hd) return;
    if (strlen(hd) >= HANDLE_CAP) return;
    bool first = !handle[0];
    strcpy(handle, hd);
    handleAt = now();
    if (first && verbose) tagLine("<- devam anahtarı alındı (%u karakter)", (unsigned)strlen(hd));
  }

  void onUsage(long totalTokens) override { tokens = totalTokens; }

  void onSessionEnd(const char *reason, float seconds) override {
    bool asked = closing;
    closing = false;
    char how[96];
    snprintf(how, sizeof(how), "kapandı (%s)", reason);
    if (!asked) tagLine("<- sunucu oturumu kapattı: %s", reason);
    accountSession(seconds, how);
  }

  void onOther(const char *kind, const char *raw, size_t len) override {
    if (!strcmp(kind, "boş")) {           // içi boş {} mesajı: sayılır, yazılmaz
      if (!emptyMsgs++ && verbose) tagLine("<- boş mesaj ({}). Sunucu bunlardan sık yolluyor; sayılıp geçilecek.");
      return;
    }
    if (!strcmp(kind, "söz sırası")) {    // modelin tepki süresi (sunucunun saatiyle): yalnızca ayrıntılı kayıtta
      if (verbose) tagLine("<- söz sırası: %s", raw);
      return;
    }
    if (!strcmp(kind, "arka model")) {    // devredilen işin ne kadar sürdüğü de yazılır
      uint32_t n = now();
      if (!strncmp(raw, "soru", 4)) { backendAt = n; tagLine("<- arka model: %s", raw); }
      else if (backendAt) {
        tagLine("<- arka model: %s (%.1f sn sürdü)", raw, since(n, backendAt) / 1000.0f);
        backendAt = 0;
        armIdle(n);                       // cevap şimdi söylenecek: uyku sayacı baştan
      }
      else tagLine("<- arka model: %s", raw);
      return;
    }
    if (!strcmp(kind, "olay")) {          // bu bağlantıda ilk kez görülen olay türü: yalnızca ayrıntılı kayıtta
      if (verbose) tagLine("<- ilk kez: %.260s%s", raw, len > 260 ? "..." : "");
      return;
    }
    if (!strcmp(kind, "bilinmeyen") && ++unknownLogged > 12) {
      if (unknownLogged == 13) tagLine("<- tanınmayan mesajlar artık yazılmayacak");
      return;
    }
    tagLine("<- %s: %.300s%s", kind, raw, len > 300 ? "..." : "");
  }

  void onError(const char *message) override {
    if (state == LINK_CONNECTING) {       // kurulum reddi: ayar merdiveni karar verir
      strncpy(setupErr, message, sizeof(setupErr) - 1);
      setupErr[sizeof(setupErr) - 1] = 0;
      setupFailed = true;
      return;
    }
    tagLine("<- sunucu hatası: %s", message);
    if (strstr(message, "arka model cevap veremedi")) backendAt = 0;
    if (P->fullDuplex && level == 0 && strstr(message, "arka model cevap veremedi")) {
      // Hızlandırma ayarları oturum açılırken kabul edilmiş ama arka model çalışırken hata verdi:
      // sonraki oturum bunlarsız açılsın.
      level = 1;
      tagLine("arka modelin hızlandırma ayarları bırakıldı (sonraki oturumdan itibaren)");
    }
  }

  // ---------------------------------------------------------------- bağlanma
  // Sunucu kurulumu kabul etmedi: sebebe göre bir şeyi bırakıp yeniden dene ya da vazgeç.
  void setupRejected(unsigned code, const char *reason) {
    tagLine("kurulum reddedildi: kod %u, sebep: %s", code, reason[0] ? reason : "(yazmıyor)");
    if (isFatalReason(reason)) { fatal(reason); return; }
    bool voiceReason = !voiceOff && level <= 1 && containsNoCase(reason, "voice");
    if (usedHandle) {
      handle[0] = 0;
      tagLine("devam anahtarı bırakıldı, yeni oturum açılıyor");
    } else if (usedHistory && !voiceReason && !containsNoCase(reason, "delegation") && !containsNoCase(reason, "tool")) {
      historyOff = true;
      tagLine("konuşma geçmişi bırakıldı, geçmişsiz deneniyor");
    } else if (voiceReason) {
      voiceOff = true;
      tagLine("\"%s\" sesi kabul edilmedi, varsayılan sesle deneniyor", endpoint().voice);
    } else if (level < P->maxLevel) {
      level++;
      vadSeen = false;                    // sade ayarda sunucu konuşma işareti yollamayabilir
      tagLine("daha sade ayarla deneniyor (basamak %d)", level);
    } else {
      fatal(reason[0] ? reason : "sunucu kurulumu kabul etmedi");
      return;
    }
    phase = PH_OPEN;
    retryAt = now() + 500;
  }

  void connectStep() {
    uint32_t n = now();
    if (reqSleep) { reqSleep = false; tagLine("vazgeçildi"); toIdle(); return; }
    if (!netUp) { fatal("WiFi yok"); return; }

    if (phase == PH_OPEN) {
      if (!after(n, retryAt)) return;
      if (attempts >= 6) { fatal("sunucuya bağlanılamadı"); return; }
      attempts++;
      Endpoint ep = endpoint();
      if (!ep.key[0] || strstr(ep.key, "BURAYA")) {
        char b[96];
        snprintf(b, sizeof(b), "secrets.h içinde %s yok", ep.keyName);
        fatal(b);
        return;
      }
      if (handle[0] && since(n, handleAt) > HANDLE_MAX_AGE_MS) handle[0] = 0;
      usedHandle = P->hasResume && level == 0 && handle[0];
      static ChatLine histLines[GPTLIVE_HISTORY_LINES];
      usedHistoryN = P->fullDuplex && level <= 1 && !historyOff ? histCollect(histLines) : 0;
      usedHistory = usedHistoryN > 0;
      char histNote[48] = "";
      if (usedHistory) snprintf(histNote, sizeof(histNote), ", önceki konuşmadan %d satırla", usedHistoryN);
      tagLine("bağlanıyor: model %s, ses %s, ayar basamağı %d%s%s", ep.model, voiceOff ? "(varsayılan)" : ep.voice, level,
           usedHandle ? ", kaldığı yerden" : "", histNote);

      static char path[320], hdrs[320];
      P->buildPath(path, sizeof(path), ep.key, ep.model);
      size_t hl = P->buildHeaders ? P->buildHeaders(hdrs, sizeof(hdrs), ep.key) : 0;
      hdrs[hl] = 0;
      if (!ws.connect(ep.host, ep.port, path, hl ? hdrs : nullptr, 15000)) {
        tagLine("%s", ws.error());
        if (ws.httpBody()[0]) tagLine("sunucunun yanıtı: %.380s", ws.httpBody());
        int st = ws.httpStatus();
        char b[160];
        if (st == 401) { snprintf(b, sizeof(b), "API anahtarı reddedildi (secrets.h içindeki %s)", ep.keyName); fatal(b); return; }
        if (st == 403) { snprintf(b, sizeof(b), "API anahtarı reddedildi ya da hesabın bu modele erişimi yok (%s; sunucunun yanıtı üstte)", ep.keyName); fatal(b); return; }
        if (st == 400) { snprintf(b, sizeof(b), "istek reddedildi (HTTP 400): %s ya da model adı; sunucunun yanıtı üstte", ep.keyName); fatal(b); return; }
        if (st == 404) { fatal("adres ya da model bulunamadı (HTTP 404)"); return; }
        if (st == 429) { fatal("kota doldu (HTTP 429)"); return; }
        retryAt = now() + (attempts <= 2 ? 400 : 1500);
        return;
      }
      if (verbose) tagLine("bağlandı (%u ms), kurulum gönderiliyor", (unsigned)(now() - n));

      P->reset();
      setupFailed = false;
      unknownLogged = 0;
      ModelConfig c;
      c.model = ep.model;
      c.voice = voiceOff ? "" : ep.voice;
      sysText[0] = 0;
      if (h.systemText) h.systemText(sysText, SYS_CAP);
      c.systemText = sysText;
      c.resumeHandle = usedHandle ? handle : "";
      c.backendModel = GPTLIVE_BACKEND_MODEL;
      c.backendNote = GPTLIVE_BACKEND_NOTE;
      if (h.backendText && backendBuf) {
        backendBuf[0] = 0;
        h.backendText(backendBuf, BACKEND_CAP);
        if (backendBuf[0]) c.backendNote = backendBuf;
      }
      c.toolsJson = h.toolsJson ? h.toolsJson : "";
      c.webSearch = GPTLIVE_WEB_SEARCH;
      c.backendTier = GPTLIVE_BACKEND_TIER;
      c.backendEffort = GPTLIVE_BACKEND_EFFORT;
      c.history = histLines;
      c.historyCount = usedHistoryN;
      c.level = level;
      size_t len = P->buildSetup((char *)ws.txPayload(), ws.txPayloadCap(), c);
      if (!len || !ws.send(WsClient::OP_TEXT, len)) {
        tagLine("kurulum gönderilemedi");
        ws.close();
        retryAt = now() + 1500;
        return;
      }
      setupOk = false;
      setupDeadline = now() + 10000;
      lastTxAt = now();
      phase = PH_SETUP;
      return;
    }

    int r = ws.poll();
    if (r == 1) {
      P->handle((const char *)ws.msg(), ws.msgLen(), *this);
      ws.consume();
      if (setupFailed) {                  // sunucu bağlantıyı kapatmadan hata mesajıyla reddetti (OpenAI)
        setupFailed = false;
        ws.close();
        setupRejected(0, setupErr);
        return;
      }
      if (setupOk) {
        tagLine("hazır%s. Konuşabilirsin.", resuming ? " (bağlantı yenilendi)" : "");
        prebufMs = P->fullDuplex ? GPTLIVE_PREBUF_MS : SPK_PREBUF_MS;
        spkSetPrebufMs(prebufMs);
        netLate = modelPauses = 0;
        rxPrevAt = 0;
        spkSetBoostDb(SPK_BOOST_DB_GPTLIVE);
        sessOpen = P->fullDuplex;         // süre ücreti yalnızca bu tür sağlayıcıda izlenir
        sessStartAt = now();
        sessUserSpoke = false;
        sessFrames = 0;
        stableSince = now();
        genDone = turnDone = true;
        enterListen(false);
        if (bargeIn && P->fullDuplex && h.echoCancel) {
          h.echoCancel(true);               // mikrofon görevi bir sonraki okumada kurar; kurulamazsa eski davranış
          if (!resuming) logLine("[bağ] araya girme açık: Nova konuşurken de dinliyorum (yankı silme)");
        }
        if (!resuming && h.readyChime) {
          // "Şimdi konuşabilirsin" sesi: çalarken mikrofon modele gitmez (GPT-Live'da yerine sessizlik gider)
          uint32_t ms = h.readyChime();
          micOpenAt = now() + ms + 60;
        }
      }
      return;
    }
    if (r < 0) {
      setupRejected(ws.closeCode(), ws.closeReason());
      return;
    }
    if (after(n, setupDeadline)) {
      tagLine("kurulum yanıtı gelmedi, yeniden deneniyor");
      ws.close();
      phase = PH_OPEN;
      retryAt = now() + 1000;
    }
  }

  // ---------------------------------------------------------------- oturum
  void onClosed() {
    const char *reason = ws.closeReason();
    tagLine("bağlantı kapandı: kod %u, sebep: %s (%s)", (unsigned)ws.closeCode(),
         reason[0] ? reason : "(yazmıyor)", ws.error());
    flushLines();
    spkFlush();
    if (P->fullDuplex) accountSession(0, "koptu");
    if (isFatalReason(reason)) { fatal(reason); return; }
    if (reconnects >= 2) { fatal("bağlantı sürekli kopuyor"); return; }
    reconnects++;
    beginConnect(true);
  }

  // Mikrofon çerçevesi (MIC_FRAME_SAMPLES örnek, PSRAM'de): oturumda, bağlanırken ve uykuda aynı
  // görevden sırayla kullanılır
  int16_t *frame = nullptr;

  void pumpMic(uint32_t n) {
    bool open = (state == LINK_LISTEN || state == LINK_THINK || (state == LINK_SPEAK && spkQuiet)) && after(n, micOpenAt) && ws.isOpen();
    // Araya girme açık ve yankı silme çalışıyorsa Nova konuşurken de mikrofon gider (yankısı silinmiş ve
    // kapıdan geçmiş ses: Nova'nın kendi sesi yerine sessizlik, kullanıcı konuşunca onun sesi)
    if (state == LINK_SPEAK && ws.isOpen() && duplexNow()) open = true;
    if (open && !micOk) {
      if (!micWarned) {
        micWarned = true;
        logLine("[mik] UYARI: mikrofonlardan ses gelmiyor, model seni duyamaz. Yazılı deneme: 's'. Arıza bulma: 'c'.");
        uiText("nova", "Mikrofonlardan ses gelmiyor.", true);
      }
      open = false;
    } else if (open && micWarned) {
      micWarned = false;
      logLine("[mik] mikrofon geri geldi, dinliyorum");
      uiText("nova", "Mikrofon geri geldi.", true);
    }
    for (int i = 0; i < 4 && micReadFrame((uint8_t *)frame); i++) {
      if (!open) {
        // Kapalıyken çerçeveler atılır: kuyrukta eski ses birikmesin. GPT-Live'da akış kesilmez
        // (oturumun saati giriş sesidir): mikrofonun yerine sessizlik gider.
        if (!P->fullDuplex || !ws.isOpen()) continue;
        sessFrames++;
        // Nova konuşurken karşıdan yoğun ses (64 KB/s) gelir; aynı anda 64 KB/s sıfır çerçevesi
        // göndermek TCP alım penceresini tıkayabiliyor (2.8.3 İngilizce kaydı: 2,5 sn boşluk).
        // Hoparlör tamponu dolarken hiç gönderilmez, normalde 4 çerçevede 1 gönderilir.
        if (state == LINK_SPEAK && !spkQuiet && ((!genDone && spkBufferedMs() < (uint32_t)prebufMs) || (++speakSilentSkip & 3) != 0)) {
          silentFrames++;
          continue;
        }
        memset(frame, 0, MIC_FRAME_BYTES);
        size_t zl = P->buildAudio((char *)ws.txPayload(), ws.txPayloadCap(), frame, MIC_FRAME_SAMPLES);
        if (!zl || !ws.send(WsClient::OP_TEXT, zl)) { onClosed(); return; }
        silentFrames++;
        lastTxAt = n;
        if (state == LINK_SPEAK) break;   // konuşurken adım başına en çok 1 gönderim: önce gelen ses okunsun
        continue;
      }
      int32_t peak = 0;
      for (int j = 0; j < MIC_FRAME_SAMPLES; j++) {
        int32_t v = ((int32_t)frame[j] * MIC_UPLINK_GAIN_Q8) >> 8;
        // Yumuşak tavan (2.8.7): yüksek sesle konuşunca tepe kırpılmasın (2.8.5 kaydı: tepe 32768, 48 çerçeve)
        if (v > 24000) v = 24000 + ((v - 24000) >> 2);
        else if (v < -24000) v = -24000 + ((v + 24000) >> 2);
        if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
        frame[j] = (int16_t)v;
        if (v < 0) v = -v;
        if (v > peak) peak = v;
      }
      // Nova konuşurken yankı kapısı kapalıysa (peak == 0, kullanıcı susuyor) sıfır çerçeveleri seyreltilir;
      // kullanıcı konuşmaya başladığı anda (peak > 0) her çerçeve gecikmesiz gider.
      if (P->fullDuplex && state == LINK_SPEAK && !spkQuiet && peak == 0) {
        if ((!genDone && spkBufferedMs() < (uint32_t)prebufMs) || (++speakSilentSkip & 3) != 0) {
          silentFrames++;
          sessFrames++;
          continue;
        }
      } else {
        speakSilentSkip = 0;
      }
      // Sırayı model belirler; mikrofon düşünürken de akmaya devam eder.
      bool loud = peak > MIC_ACTIVITY_PEAK;
      if ((uint32_t)peak > txPeakMax) txPeakMax = (uint32_t)peak;   // durum satırı: modele giden ses (yankı silme ve kapıdan sonra)
      if (loud) txLoudFrames++;
      if (P->fullDuplex) {
        // Konuşma olup olmadığına gelen yazı karar verir (onText). Yerel ses seviyesi klavye
        // tıkırtısını konuşmadan ayıramaz: uyku sayacına ve ekrana karışmaz, yalnızca kaba gecikme
        // ölçümünde kullanılır (2.2.8'de klavye sesi oturumu 75 sn açık tutmuştu).
        if (loud) { localSpeechAt = n; localLoudAt = n; }
      } else if (vadSeen) {
        // Sunucu "konuşma var" dediği sürece yereldeki son yüksek ses, sustuğun an sayılır
        // (yalnızca gecikme ölçümü için). Ekran ve uyku sayacı yerel sese bakmaz.
        if (loud && vadActive) localSpeechAt = n;
      } else if (loud) {
        // Yedek yol: sunucu konuşma işareti yollamıyorsa yerel tahmin kullanılır.
        idleDeadline = n + LINK_IDLE_SLEEP_MS;        // quietDeadline'a dokunmaz: tıkırtı sonsuza dek uyanık tutamaz
        localSpeechAt = n;
        heardSpeech = true;
        quietFrames = 0;
        if (state == LINK_THINK) state = LINK_LISTEN;
      } else if (heardSpeech && state == LINK_LISTEN && ++quietFrames >= 6) {
        state = LINK_THINK;
        thinkSince = n;
      }
      size_t len = P->buildAudio((char *)ws.txPayload(), ws.txPayloadCap(), frame, MIC_FRAME_SAMPLES);
      if (!len || !ws.send(WsClient::OP_TEXT, len)) { onClosed(); return; }
      sentFrames++;
      sessFrames++;
      lastTxAt = n;
      if (state == LINK_SPEAK) break;     // konuşurken adım başına en çok 1 gönderim: gelen ses bekletilmesin
    }
  }

  void sessionStep() {
    uint32_t n = now();

    // 1) gelen mesajlar
    for (int i = 0; i < 24; i++) {
      int r = ws.poll();
      if (r == 0) break;
      if (r < 0) { onClosed(); return; }
      // Çalma tamponunda yer yoksa mesaj bekletilir; TCP gönderimi kendiliğinden yavaşlar.
      if (!dropTurn && spkFree() < protoMaxAudioBytes(ws.msgLen())) {
        if (!blocked) { blocked = true; backpressure++; }
        break;
      }
      blocked = false;
      if (ws.msgTruncated()) tagLine("uyarı: %u bayttan büyük mesaj kesildi", (unsigned)WS_RX_BYTES);
      P->handle((const char *)ws.msg(), ws.msgLen(), *this);
      ws.consume();
      if (state == LINK_IDLE || state == LINK_CONNECTING) return;
    }
    n = now();                            // mesajlar işlenirken zaman damgaları güncellendi

    // 2) komutlar
    if (reqSleep) {
      reqSleep = false;
      tagLine("oturum kapatıldı");
      spkFlush();
      toIdle();
      return;
    }
    if (reqInterrupt) {
      reqInterrupt = false;
      if (state == LINK_SPEAK) {
        logLine("[nova]  söz kesildi (buton)");
        uint32_t heard = playedMs();                         // kullanıcının gerçekten duyduğu kadarı
        spkFlush();
        dropCaptions();
        // Sağlayıcı destekliyorsa üretimi durdur ve cevabı duyulan yerde kırp (OpenAI)
        for (int part = 0; part < 4 && ws.isOpen(); part++) {
          size_t len = P->buildInterrupt((char *)ws.txPayload(), ws.txPayloadCap(), part, heard);
          if (!len) break;
          ws.send(WsClient::OP_TEXT, len);
        }
        if (!genDone) { dropTurn = true; dropSince = n; }   // turun kalan sesi gelince atılacak
        flushLines();
        enterListen(true);
      }
    }
    if (reqVoiceCut) {
      reqVoiceCut = false;
      if (state == LINK_SPEAK && !spkQuiet) {
        // Model kullanıcıyı duydu ve susacak; cihazın tamponunda bekleyen (1-2 sn'ye varabilen) ses atılır.
        // Turun hâlâ gelen sesi akış durana dek atılır. Mikrofon zaten açık: bekleme konmaz.
        voiceCuts++;
        logLine("[nova]  söz kesildi (araya girdin; tamponda %u ms ses atıldı)", (unsigned)spkBufferedMs());
        spkFlush();
        dropCaptions();
        if (!genDone) { dropTurn = true; dropSince = n; }
        flushLine(false);
        enterListen(false);
      }
    }
    if (pendingText[0] && (state == LINK_LISTEN || state == LINK_THINK)) sendPendingText();
    if (pendingNote[0] && (state == LINK_LISTEN || state == LINK_THINK)) sendPendingNote();
    if (toolN) runTools(n);
    if (dropTurn && since(n, dropSince) > 15000) dropTurn = false;
    if (P->fullDuplex) {
      // "Cevap bitti" işareti gelmez: kesilen turun sesi, akış durunca atılmış sayılır.
      if (dropTurn && since(n, dropSince) > GPTLIVE_END_GAP_MS && since(n, rxLoudAt) > GPTLIVE_END_GAP_MS) dropTurn = false;
      // Yazıların da bitiş işareti yok: bir süre yeni parça gelmeyen ve mikrofonda konuşma sürmeyen satır Serial'e basılır.
      if (lineBuf[1][0] && since(n, lineAt[1]) > GPTLIVE_THINK_AFTER_MS && (!localLoudAt || since(n, localLoudAt) > 600)) flushLine(true);
      if (state != LINK_SPEAK && lineBuf[0][0] && since(n, lineAt[0]) > 1200) flushLine(false);
      // Sunucunun cihazdan beklediği yanıt varsa gönder (arka model yokken devredilen iş)
      if (P->buildPending && ws.isOpen()) {
        for (int k = 0; k < 4; k++) {
          size_t len = P->buildPending((char *)ws.txPayload(), ws.txPayloadCap());
          if (!len) break;
          ws.send(WsClient::OP_TEXT, len);
        }
      }
    }

    // 3) duruma göre
    if (state == LINK_SPEAK) {
      showCaptions(false);
      bool ended = audioTakePlayEnd();
      audioTakePlayStart();
      if (P->fullDuplex && !genDone && since(n, speakAt) > GPTLIVE_NOISE_CHECK_MS && since(n, lineAt[0]) > GPTLIVE_NOISE_CHECK_MS &&
          since(n, winAt) < 2000 && (winMax > winPrevMax ? winMax : winPrevMax) < GPTLIVE_NOISE_MAX_RMS) {
        // Ses "konuşma" eşiğini aşıyor ama düşük seviyeli ve yazısı yok: sunucu susarken hışırtı
        // yolluyor demektir. Eşik yükseltilir; yoksa cihaz hep "konuşuyor" kalır, mikrofon açılmaz.
        uint32_t lvl = winMax > winPrevMax ? winMax : winPrevMax;
        if (lvl * 2 > outSilenceRms) outSilenceRms = lvl * 2;
        tagLine("not: %u sn'dir yazısı olmayan, düşük seviyeli ses geliyor (seviye %u). Konuşma sayılmayacak; sessizlik eşiği %u yapıldı.",
                (unsigned)(GPTLIVE_NOISE_CHECK_MS / 1000), (unsigned)lvl, (unsigned)outSilenceRms);
        genDone = turnDone = true;
        spkFlush();
        finishSpeak();
        return;
      }
      if (P->fullDuplex && !genDone && since(n, lastAudioAt) > GPTLIVE_END_GAP_MS) {
        // Bitiş işareti gelmez: bir süredir sesli parça gelmediyse cevap bitmiş sayılır.
        // Tampon sıfırlanmaz (spkFlush çağrılmaz): model cümle ortasında duraksadıysa yeni ses geldiğinde
        // ön tamponu baştan doldurmak zorunda kalmadan kesintisiz devam eder.
        genDone = turnDone = true;
        uint32_t tailMs = (uint32_t)((uint64_t)tailSilentBytes * 1000 / (2 * SPK_RATE));
        spkMarkEnd();
        if (spkBufferedMs() <= tailMs) speakerQuiet(n);
      }
      if (P->fullDuplex) {
        if (ended && genDone && !spkQuiet) speakerQuiet(n);
        // Hoparlör sustu ve yeterince uzun süredir ses gelmiyor: cevap bitti.
        if (spkQuiet && (userCutPause || since(n, lastAudioAt) > GPTLIVE_ANSWER_END_MS)) { finishSpeak(); return; }
      } else if (ended && genDone) { finishSpeak(); return; }
      if (genDone && spkBufferedMs() == 0 && since(n, lastAudioAt) > 2500) { finishSpeak(); return; }
      if (!genDone && since(n, lastAudioAt) > 15000) {
        tagLine("cevap yarım kaldı (15 sn ses gelmedi)");
        spkMarkEnd();
        genDone = true;
        finishSpeak();
        return;
      }
    } else {
      if (goAwayPending && state == LINK_LISTEN && turnDone && !vadActive) {
        goAwayPending = false;
        tagLine("bağlantı yenileniyor");
        ws.close();
        beginConnect(true);
        return;
      }
      if (byePending && since(n, byeAt) > LINK_BYE_WAIT_MS) { byeClose(); return; }      // vedaya cevap gelmedi
      if (P->fullDuplex && state == LINK_LISTEN && userTurnOpen && since(n, lineAt[1]) > GPTLIVE_THINK_AFTER_MS &&
          (!localLoudAt || since(n, localLoudAt) > 600)) {
        state = LINK_THINK;               // kullanıcının yazısı ve sesi durdu: cevap bekleniyor
        thinkSince = n;
        userTurnOpen = false;
      }
      // Sunucu "konuşma sürüyor" diyorsa uyku sayacı bekler; işaret takılı kalırsa üst sınır keser.
      // Arka model bir işi sürdürürken (arama 10-20 sn sürebiliyor) oturum sessizlik yüzünden kapatılmaz.
      bool backendWait = P->fullDuplex && backendAt && since(n, backendAt) < LINK_BACKEND_WAIT_MS;
      if (state == LINK_THINK && !backendWait && since(n, thinkSince) > LINK_THINK_MAX_MS) backToListen();
      // Kısa kapanış: cevap soruyla bitmedi, ardından kimse konuşmadı. Mikrofonda yeni bir ses varsa (konuşmaya
      // başlamış olabilir, yazısı henüz gelmedi) biraz daha beklenir; klavye bunu en çok normal uyku süresine uzatır.
      if (shortDeadline && state == LINK_LISTEN && !backendWait && !userTurnOpen && !vadActive && after(n, shortDeadline)) {
        if (localLoudAt && since(n, localLoudAt) < 1500) {
          shortDeadline = n + 1000;
        } else {
          tagLine("cevap soruyla bitmedi ve %u sn konuşulmadı, oturum kapatıldı. %s",
                  (unsigned)((since(n, armedAt) + 500) / 1000), wakeHint());
          toIdle();
          return;
        }
      }
      if (state == LINK_LISTEN && !backendWait && (after(n, quietDeadline) || (!vadActive && after(n, idleDeadline)))) {
        tagLine("%u sn konuşma yok, oturum kapatıldı. %s", (unsigned)((since(n, armedAt) + 500) / 1000), wakeHint());
        toIdle();
        return;
      }
    }
    if (reconnects && since(n, stableSince) > 60000) reconnects = 0;

    // 4) mikrofon
    pumpMic(n);

    // 5) bağlantıyı canlı tut
    if (ws.isOpen() && since(now(), lastTxAt) > 15000) { ws.ping(); lastTxAt = now(); }
    // Oturumda 2 sn'de bir ping: ses takılınca gidiş-dönüş süresi de uzuyorsa yol (ağ) yavaştır;
    // süre normalken ses gelmiyorsa sunucu (model) yavaştır. Durum satırına yazılır.
    if (sessOpen && ws.isOpen() && state != LINK_CONNECTING && since(now(), pingAt) >= 2000) { pingAt = now(); ws.ping(); }
  }

  void drainMic() {
    for (int i = 0; i < 4 && micReadFrame((uint8_t *)frame); i++) {}
  }

  void wakeDisarm() {
    if (wakeArmed && h.wakeArm) h.wakeArm(false);
    wakeArmed = false;
  }

  // Uykuda mikrofon: uyandırma sözü açıksa motora verilir, değilse atılır.
  // WiFi bağlanmadan (netUp) motor kurulmaz: açılışta ağ servisleri kurulurken iç RAM'i ortadan bölmesin.
  void idleMic() {
    uint32_t n = now();
    if (reqWakeRearm) { reqWakeRearm = false; wakeDisarm(); }
    bool use = h.wakeFeed && h.wakeArm && wakeEnabled && netUp && micOk && !hold && !netBusy;
    if (!use && wakeArmed) wakeDisarm();
    if (use && !wakeArmed && after(n, wakeArmAt)) {
      wakeArmed = h.wakeArm(true);
      if (!wakeArmed) wakeArmAt = n + 30000;     // kurulamadı: bir süre sonra yeniden dene
    }
    for (int i = 0; i < 4 && micReadFrame((uint8_t *)frame); i++) {
      if (!use || !wakeArmed) continue;
      for (int j = 0; j < MIC_FRAME_SAMPLES; j++) {     // modele giden sesle aynı seviye
        int32_t v = ((int32_t)frame[j] * MIC_UPLINK_GAIN_Q8) >> 8;
        frame[j] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
      }
      if (h.wakeFeed(frame, MIC_FRAME_SAMPLES)) {
        wakeHits++;
        logLine("[uyan] \"Hey Nova\" duyuldu (açılıştan beri %u. kez), bağlanılıyor", (unsigned)wakeHits);
        reqWake = true;
        break;
      }
    }
  }

  void loop() {
    if (reqNote) {
      reqNote = false;
      strncpy(pendingNote, noteBuf, sizeof(pendingNote) - 1);
      pendingNote[sizeof(pendingNote) - 1] = 0;
      if (state == LINK_IDLE) reqWake = true;
    }
    if (reqSay) {
      reqSay = false;
      strncpy(pendingText, sayBuf, sizeof(pendingText) - 1);
      pendingText[sizeof(pendingText) - 1] = 0;
      if (state == LINK_IDLE) reqWake = true;
    }
    switch (state) {
      case LINK_IDLE:
        idleMic();
        reqSleep = false;
        reqInterrupt = false;
        if (hold) reqWake = false;          // kablosuz yükleme sürüyor: bağlanılmaz
        if (reqWake) {
          reqWake = false;
          wakeDisarm();                     // uyandırma motoru kalkar: iç RAM şifreli bağlantıya kalsın
          if (h.beforeConnect) h.beforeConnect();
          beginConnect(false);
        }
        break;
      case LINK_CONNECTING:
        drainMic();
        reqWake = false;
        reqInterrupt = false;
        if (hold) { tagLine("bakım (kablosuz yükleme): bağlantı bırakıldı"); toIdle(); break; }
        connectStep();
        break;
      default:
        reqWake = false;
        if (hold) { tagLine("bakım (kablosuz yükleme): oturum kapatıldı"); toIdle(); break; }
        sessionStep();
        break;
    }
  }
};

Link L;

}  // namespace

bool linkBegin(WsTransport *t, const LinkHooks &hooks) {
  L.t = t;
  L.h = hooks;
  uint8_t *rx = (uint8_t *)hooks.bigAlloc(WS_RX_BYTES);
  uint8_t *tx = (uint8_t *)hooks.bigAlloc(WS_TX_BYTES);
  L.sysText = (char *)hooks.bigAlloc(SYS_CAP);
  L.backendBuf = (char *)hooks.bigAlloc(BACKEND_CAP);
  L.toolQ = (Link::ToolCall *)hooks.bigAlloc(sizeof(Link::ToolCall) * TOOLQ_N);
  L.toolOut = (char *)hooks.bigAlloc(TOOL_OUT_CAP);
  L.handle = (char *)hooks.bigAlloc(HANDLE_CAP);
  L.hist = (Link::Hist *)hooks.bigAlloc(sizeof(Link::Hist) * GPTLIVE_HISTORY_LINES);
  // Büyük tamponlar PSRAM'de (bigAlloc): iç RAM şifreli bağlantıya ve WiFi'ye kalsın
  L.caps = (Link::Caption *)hooks.bigAlloc(sizeof(Link::Caption) * Link::CAP_N);
  L.lineBuf = (char (*)[640])hooks.bigAlloc(sizeof(L.lineBuf[0]) * 2);
  L.frame = (int16_t *)hooks.bigAlloc(MIC_FRAME_BYTES);
  if (!rx || !tx || !L.sysText || !L.backendBuf || !L.toolQ || !L.toolOut || !L.handle || !L.hist || !L.caps || !L.lineBuf || !L.frame) return false;
  L.sysText[0] = 0;
  L.handle[0] = 0;
  L.lineBuf[0][0] = L.lineBuf[1][0] = 0;
  L.ws.begin(t, rx, WS_RX_BYTES, tx, WS_TX_BYTES, hooks.nowMs, hooks.rnd, hooks.idle);
  gptlive::setFirstEventLog(GPTLIVE_LOG_FIRST_EVENTS);
  return true;
}

void linkLoop() { L.loop(); }

const char *linkProviderName() { return "OpenAI (GPT-Live)"; }
const char *linkModelName() { return L.endpoint().model; }
bool linkKeyPresent() {
  const char *k = OPENAI_API_KEY;
  return k[0] && !strstr(k, "BURAYA") && strlen(k) >= 20;
}
void linkSetNetUp(bool up) { L.netUp = up; }
void linkSetMicOk(bool ok) { L.micOk = ok; }
void linkWake() { if (L.state == LINK_IDLE) L.reqWake = true; }
void linkSleep() { if (L.state != LINK_IDLE) L.reqSleep = true; }

void linkButton() {
  switch (L.state) {
    case LINK_IDLE:  L.reqWake = true; break;
    case LINK_SPEAK: L.reqInterrupt = true; break;
    default:         L.reqSleep = true; break;
  }
}

void linkNotify(const char *text) {
  strncpy(L.noteBuf, text, sizeof(L.noteBuf) - 1);
  L.noteBuf[sizeof(L.noteBuf) - 1] = 0;
  L.reqNote = true;
}

void linkSay(const char *text) {
  strncpy(L.sayBuf, text, sizeof(L.sayBuf) - 1);
  L.sayBuf[sizeof(L.sayBuf) - 1] = 0;
  L.reqSay = true;
}

void linkSetVerbose(bool on) { L.verbose = on; }
void linkSetWakeEnabled(bool on) { L.wakeEnabled = on; }
void linkWakeRearm() { L.reqWakeRearm = true; }
void linkSetHold(bool on) { L.hold = on; }
void linkSetNetBusy(bool on) { L.netBusy = on; }
void linkSetBargeIn(bool on) { L.bargeIn = on; }
bool linkBargeIn() { return L.bargeIn; }
void linkSetVoice(const char *voice) {
  L.voiceOv = voice;
  L.voiceOff = false;                   // yeni ses: sunucu yeniden denesin
}
const char *linkVoiceName() { return L.voiceName(); }
bool linkVerbose() { return L.verbose; }
LinkState linkState() { return L.state; }

uint32_t linkBackendSince() {
  uint32_t b = L.backendAt;
  if (!b || L.state == LINK_IDLE || L.since(L.now(), b) >= LINK_BACKEND_WAIT_MS) return 0;
  return b;
}

const char *linkStateName() {
  switch (L.state) {
    case LINK_IDLE:       return "uyku";
    case LINK_CONNECTING: return "bağlanıyor";
    case LINK_LISTEN:     return "dinliyor";
    case LINK_THINK:      return "düşünüyor";
    case LINK_SPEAK:      return "konuşuyor";
  }
  return "?";
}

void linkPrintStats() {
  if (L.P->fullDuplex) {
    float live = L.bootSeconds + (L.sessOpen ? L.since(L.now(), L.sessStartAt) / 1000.0f : 0);
    logLine("[bağ] %s %s | tampon %u ms | alınan %u KB, %u mesaj (en büyük %u B) | giden %u çerçeve + %u sessizlik | gelen sessizlik %.0f sn (en yüksek seviye %u, eşik %u) | modele giden tepe %u (%u yüksek çerçeve) | ping %u ms, en çok %u (%u pong, %u kayıp) | tur %u | bekletme %u | taşma %u | geç gelen ses %u, Nova duraksaması %u | bağlı kalınan %.1f dk (~%.2f $)",
         L.P->name, linkStateName(), (unsigned)spkBufferedMs(), (unsigned)(L.ws.rxBytes / 1024), (unsigned)L.ws.rxMsgs,
         (unsigned)L.ws.maxMsg, (unsigned)L.sentFrames, (unsigned)L.silentFrames, L.droppedSilentBytes / (2.0f * SPK_RATE),
         (unsigned)L.silentMaxRms, (unsigned)L.outSilenceRms, (unsigned)L.txPeakMax, (unsigned)L.txLoudFrames,
         (unsigned)L.ws.rttLast, (unsigned)L.ws.rttMax, (unsigned)L.ws.pongs, (unsigned)L.ws.pingsLost, (unsigned)L.turns,
         (unsigned)L.backpressure, (unsigned)spkOverflowCount(), (unsigned)L.netLate, (unsigned)L.modelPauses, live / 60.0f,
         live / 60.0f * GPTLIVE_USD_PER_MIN);
    L.txPeakMax = L.txLoudFrames = 0;
    L.ws.rttMax = 0;
    return;
  }
  logLine("[bağ] %s %s | tampon %u ms | alınan %u KB, %u mesaj (%u boş, en büyük %u B) | giden %u çerçeve | tur %u | bekletme %u | taşma %u | jeton %ld",
       L.P->name, linkStateName(), (unsigned)spkBufferedMs(), (unsigned)(L.ws.rxBytes / 1024), (unsigned)L.ws.rxMsgs,
       (unsigned)L.emptyMsgs, (unsigned)L.ws.maxMsg, (unsigned)L.sentFrames, (unsigned)L.turns, (unsigned)L.backpressure,
       (unsigned)spkOverflowCount(), L.tokens);
}
