// Konuşma modeli sağlayıcılarının ortak arayüzü. link.cpp yalnızca bunu bilir.
// Firmware'de takılı olan GPT-Live (gptlive.cpp). Gemini Live ve OpenAI Realtime da bu arayüzle yazıldı;
// 2.5.0'dan beri firmware/arsiv/saglayicilar klasöründeler (aşağıdaki Gemini ve OpenAI alanları onlar için).
// Donanıma bağlı değil.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Önceki oturumdan taşınan konuşma satırı (yalnızca geçmişi yazı olarak alabilen sağlayıcıda)
struct ChatLine {
  bool user;
  const char *text;
};

struct ModelConfig {
  const char *model = "";
  const char *voice = "";         // boşsa sunucunun varsayılanı
  const char *systemText = "";    // kişilik ve bağlam
  // Ayar merdiveni: sunucu kurulumu reddederse bir basamak inilir (0 = hepsi, 2 = en sade)
  int level = 0;

  // --- Gemini
  const char *resumeHandle = "";  // önceki oturumun devam anahtarı (yoksa boş)
  const char *langs = "";         // yazıya dökmede dil ipucu, virgülle: "tr-TR,en-US"; boşsa gönderilmez
  int vadSilenceMs = 0;           // "söz bitti" demek için beklenen sessizlik; 0 = varsayılan

  // --- OpenAI
  const char *eagerness = "";     // semantic_vad sabrı: low / medium / high / auto
  const char *sttModel = "";      // konuşmanı yazıya döken model (boşsa yazı istenmez)
  const char *sttLang = "";       // yazıya dökme dili: "tr"

  // --- GPT-Live
  const char *backendModel = "";  // arama ve ağır düşünme işini yapan arka model (boşsa istenmez)
  const char *backendNote = "";   // arka modele verilen talimat
  bool webSearch = false;         // arka modele web araması aracı verilsin mi
  const char *backendTier = "";   // arka modelin hizmet katmanı ("priority" = hızlı kip); boşsa gönderilmez
  const char *backendEffort = ""; // arka modelin düşünme payı ("low" gibi); boşsa gönderilmez
  const ChatLine *history = nullptr;   // önceki oturumun son satırları, eskiden yeniye
  int historyCount = 0;
  // Arka modele verilen cihaz araçları: Responses "function" araçlarının JSON dizisi (boşsa yok).
  // Arka model bir aracı çağırınca onToolCall gelir; sonuç buildToolResult + buildContinue ile döner.
  const char *toolsJson = "";
};

// Sunucudan gelenler. Hepsi mesajı işleyen görevin içinde çağrılır.
struct ModelSink {
  virtual void onSetupComplete() = 0;
  virtual void onAudio(const uint8_t *pcm, size_t bytes) = 0;   // 24 kHz mono int16, çift sayıda bayt
  virtual void onText(bool fromUser, const char *text) = 0;     // yazıya dökülmüş parça
  virtual void onUserTextDone() {}                              // kullanıcının bu sözünün yazısı tamamlandı (bildiren sağlayıcılarda)
  virtual void onGenerationComplete() = 0;                      // bu cevabın sesi tamam
  virtual void onTurnComplete() = 0;
  virtual void onInterrupted() = 0;                             // sunucu, kullanıcı konuştuğu için cevabı kesti
  // offsetMs: sunucunun bildirdiği ses akışı konumu, yoksa -1
  virtual void onVoiceActivity(bool start, int32_t offsetMs) = 0;
  virtual void onGoAway(int secondsLeft) = 0;
  virtual void onResume(const char *handle, bool resumable) = 0;
  virtual void onUsage(long totalTokens) = 0;
  // Sunucu oturumu kapattığını bildirdi (GPT-Live). seconds: ücretlendirilen süre, bilinmiyorsa 0
  virtual void onSessionEnd(const char *reason, float seconds) {}
  virtual void onError(const char *message) = 0;                // bağlantı açıkken gelen hata mesajı
  // Arka model bir cihaz aracını çağırdı (GPT-Live). args: aracın JSON argümanları (yazı olarak).
  virtual void onToolCall(const char *callId, const char *name, const char *args) {}
  // kind: "toolCall", "toolCallCancellation", "error", "json-hata", "bilinmeyen" ya da
  // "boş" (içi boş {} mesajı: Gemini bunlardan sık yollar, taşıdığı bir şey yoktur)
  virtual void onOther(const char *kind, const char *raw, size_t len) = 0;
  virtual ~ModelSink() {}
};

// Bir sağlayıcının yapabildikleri. build* işlevleri yazılan bayt sayısını döner;
// sığmazsa ya da gönderilecek bir şey yoksa 0.
struct Provider {
  const char *name;       // kayıt etiketi: "gemini" / "openai" / "gptlive"
  const char *label;      // cümle içinde: "Gemini" / "GPT" / "GPT-Live"
  int maxLevel;           // ayar merdiveninin son basamağı
  bool hasResume;         // oturum devam anahtarı var mı
  // Kullanıcının sözünün yazısı cevapla aynı anda, ondan bağımsız mı geliyor? (GPT: evet; yazı
  // bitince onUserTextDone çağrılır. Gemini: hayır; yazı cevaptan önce gelir.)
  bool userTextAsync;
  // Aynı anda dinleyip konuşan model (GPT-Live): söz sırası olayı ve "cevap bitti" işareti yoktur,
  // giriş sesinin kesintisiz akmasını ister. link.cpp bu sağlayıcıda mikrofon kapalıyken sessizlik
  // yollar ve cevabın bittiğine ses akışının durmasından karar verir.
  bool fullDuplex;

  size_t (*buildPath)(char *out, size_t cap, const char *key, const char *model);     // WebSocket yolu
  size_t (*buildHeaders)(char *out, size_t cap, const char *key);                     // ek HTTP başlıkları ("Ad: değer\r\n")
  void   (*reset)();                                                                  // yeni bağlantı: iç durumu sıfırla
  size_t (*buildSetup)(char *out, size_t cap, const ModelConfig &c);
  size_t (*buildAudio)(char *out, size_t cap, const int16_t *pcm16k, size_t samples); // 16 kHz mono mikrofon
  size_t (*buildText)(char *out, size_t cap, const char *text, int part);             // yazılı soru; part 0,1,.. (0 dönünce biter)
  size_t (*buildMicPause)(char *out, size_t cap);                                     // mikrofon kapanırken
  size_t (*buildInterrupt)(char *out, size_t cap, int part, uint32_t playedMs);       // cevap yarıda kesilirken
  bool   (*handle)(const char *msg, size_t len, ModelSink &sink);                     // sunucu mesajı; JSON değilse false
  size_t (*buildClose)(char *out, size_t cap);                                        // oturumu kapatma isteği (yoksa nullptr)
  size_t (*buildPending)(char *out, size_t cap);                                      // sunucunun beklediği yanıt (yoksa nullptr; 0 dönünce biter)
  // Cihaz araçları (yoksa nullptr; arşivdeki sağlayıcılarda yok, tabloları bu alanları boş bırakır)
  size_t (*buildToolResult)(char *out, size_t cap, const char *callId, const char *output);  // aracın sonucu
  size_t (*buildContinue)(char *out, size_t cap);                                     // sonuçlar gitti: arka model devam etsin
  size_t (*buildNote)(char *out, size_t cap, const char *text);                       // cihazdan modele not (ör. zamanlayıcı çaldı)
};

// Bu uzunluktaki bir mesajdan en çok kaç bayt ses çıkabileceği (tampon yer kontrolü için)
inline size_t protoMaxAudioBytes(size_t msgLen) { return msgLen / 4 * 3 + 4; }
