// Nova'nın konuşma modeliyle (OpenAI GPT-Live) konuşma akışı.
//
//   uyku --("Hey Nova" / BOOT)--> bağlanıyor --> dinliyor <--> konuşuyor
//                                                   |
//                                                   +--(sessizlik / "görüşürüz")--> uyku
//
// GPT-Live aynı anda dinleyip konuşur; kimin sırası olduğuna model karar verir. Araya girme kapalıyken
// Nova konuşurken modele sessizlik gider. Açıkken (yankı silmeyle) mikrofon sesi de gider ve kullanıcının
// sözü yazıya dökülünce Nova'nın kalan sesi atılır.
//
// Sağlayıcıya özgü her şey (mesaj biçimleri, adres, anahtar) proto.h'deki Provider tablosunun
// arkasındadır: gptlive.cpp. Sırayla konuşan sağlayıcılar (Gemini Live, GPT Realtime) aynı arayüzle
// yazılmıştı ve firmware/arsiv/saglayicilar klasöründe duruyor.
//
// Donanıma doğrudan dokunmaz: ses için audio.h, ekran için ui.h, ağ için
// WsTransport kullanır. Bu sayede masaüstünde sahte sunucuyla sınanabiliyor.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "wsclient.h"

enum LinkState : uint8_t { LINK_IDLE, LINK_CONNECTING, LINK_LISTEN, LINK_THINK, LINK_SPEAK };

struct LinkHooks {
  uint32_t (*nowMs)() = nullptr;
  uint32_t (*rnd)() = nullptr;
  void (*idle)() = nullptr;                                   // kısa bekleme (kartta 1 ms)
  void (*systemText)(char *buf, size_t cap) = nullptr;        // kişilik + tarih/saat; her oturum açılışında çağrılır
  void *(*bigAlloc)(size_t) = nullptr;                        // büyük tamponlar (kartta PSRAM)
  // Uyandırma sözü (yoksa nullptr). Uykudayken mikrofon sesi wakeFeed'e verilir, true dönerse uyanılır.
  // wakeArm(true) motoru kurar (uykuya geçince), wakeArm(false) kaldırır (uyanırken: iç RAM bağlantıya kalsın).
  bool (*wakeArm)(bool on) = nullptr;
  bool (*wakeFeed)(const int16_t *pcm, size_t n) = nullptr;
  // Oturum hazır olunca çalınan kısa ses ("şimdi konuşabilirsin"); süresini (ms) döner. Bu süre boyunca
  // mikrofon modele verilmez. nullptr: ses yok.
  uint32_t (*readyChime)() = nullptr;
  // Bağlanmadan hemen önce çağrılır (iç RAM durumunu kayda yazmak için). nullptr: yok.
  void (*beforeConnect)() = nullptr;
  // Süreyle ücretlenen oturum (GPT-Live) kapandı: ücretlenen saniye ve kullanıcının konuşup konuşmadığı
  // (yazılı soru da sayılır). Kalıcı sayaç için. nullptr: yok.
  void (*usage)(float seconds, bool userSpoke) = nullptr;
  // Yankı silme (araya girme açıkken, GPT-Live'da): oturum hazır olunca true, kapanınca false.
  // echoActive: yankı silme gerçekten çalışıyor mu (kurulamadıysa false; o zaman eski davranış).
  void (*echoCancel)(bool on) = nullptr;
  bool (*echoActive)() = nullptr;
  // Cihaz araçları: arka modelin kullanabileceği işlevlerin JSON tanımı (proto.h ModelConfig::toolsJson)
  // ve çağrıyı yürüten kanca. tool: aracı çalıştırır, sonucu out'a yazar (model bunu okur). Ağ görevinden
  // çağrılır, kısa sürmeli. nullptr: araç yok.
  const char *toolsJson = nullptr;
  void (*tool)(const char *name, const char *args, char *out, size_t cap) = nullptr;
  // Kullanıcının yazıya dökülmüş her tam sözü (yerel komut yedekleri için: "sesi kıs" gibi). nullptr: yok.
  void (*heard)(const char *text) = nullptr;
  // Konuşmanın her tam satırı (kullanıcı ve Nova); oturum sonu not çıkarma için. nullptr: yok.
  void (*line)(bool user, const char *text) = nullptr;
  // Arka modelin talimatı (tarih/saat ve araç kuralları eklenmiş); her oturum açılışında çağrılır.
  // nullptr: config.h'deki GPTLIVE_BACKEND_NOTE.
  void (*backendText)(char *buf, size_t cap) = nullptr;
};

bool linkBegin(WsTransport *t, const LinkHooks &hooks);
void linkLoop();                  // ağ görevinden sürekli çağrılır

// Aşağıdakiler başka görevden çağrılabilir
void linkSetNetUp(bool up);
void linkSetMicOk(bool ok);       // mikrofonlar çalışmıyorsa ses modele gönderilmez
void linkButton();                // uykudaysa uyandırır, konuşuyorsa sözünü keser, dinliyorsa uyutur
void linkWake();
void linkSleep();
void linkSay(const char *text);   // yazılı soru: mikrofon olmadan bağlantıyı denemek için
// Cihazdan Nova'ya not (zamanlayıcı çaldı gibi): Nova bunu kullanıcıya söyler. Uykudaysa oturum açılır.
void linkNotify(const char *text);
void linkSetVerbose(bool on);
void linkSetWakeEnabled(bool on);  // uyandırma sözünü aç/kapa (kapalıyken motor kaldırılır)
void linkWakeRearm();              // uyandırma ayarı değişti: motoru kaldırıp yeniden kur
void linkSetHold(bool on);         // bakım (kablosuz yükleme, yankı ölçümü): oturumu kapat, uyandırmayı ve bağlanmayı durdur
// Uykudayken cihaz başka bir şifreli bağlantı kuracak (hava durumu): uyandırma motoru kaldırılır (iç RAM
// bağlantıya kalsın). Bir linkLoop çağrısından sonra geçerli; bitince false.
void linkSetNetBusy(bool on);
// Ses tonu (ör. "gleam"). nullptr ya da "" = config.h'deki GPTLIVE_VOICE. Sonraki oturumda geçerli.
void linkSetVoice(const char *voice);
const char *linkVoiceName();
// Araya girme: açıkken oturumda yankı silme çalışır, Nova konuşurken de mikrofon sesi
// gider; kullanıcının sözü yazıya dökülünce Nova'nın kalan sesi kesilir. Sonraki oturumda geçerli.
void linkSetBargeIn(bool on);
bool linkBargeIn();
bool linkVerbose();
LinkState linkState();
const char *linkStateName();
uint32_t linkBackendSince();       // arka modele soru devredildiyse o an (millis), değilse 0
void linkPrintStats();

const char *linkProviderName();   // "OpenAI (GPT-Live)"
const char *linkModelName();
bool linkKeyPresent();            // secrets.h'de OPENAI_API_KEY doldurulmuş mu
