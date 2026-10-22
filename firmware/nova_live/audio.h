// Nova V2 — ses katmanı: iki mikrofon, iki amfi, çalma tamponu
#pragma once
#include <Arduino.h>
#include "config.h"

#define MIC_FRAME_BYTES (MIC_FRAME_SAMPLES * 2)

bool audioBegin();          // I2S ve görevleri başlatır
void audioStop();           // OTA / uyku öncesi her şeyi durdurur

// Mikrofon: 16 kHz mono int16 çerçeve. Çerçeve hazırsa true döner.
bool micReadFrame(uint8_t *out /* MIC_FRAME_BYTES */);

// Hoparlör: 24 kHz mono int16 PCM
size_t spkWrite(const uint8_t *data, size_t len);   // tampona ekler
void   spkFlush();          // çalmayı hemen keser
void   spkMarkEnd();        // "bu cevabın sesi bitti" işareti
void   spkSetVolume(int percent);
int    spkGetVolume();
uint32_t spkBufferedMs();
size_t   spkFree();          // tamponda kalan yer (bayt)

// Test için: sesi yalnızca sol ya da yalnızca sağ amfiye ver (normalde ikisi de açık)
void audioRoute(bool left, bool right);

// Çalma başladı / bitti olayları (bir kez okunur)
bool audioTakePlayStart();
bool audioTakePlayEnd();

// Mikrofon öz-testi (açılışta bir kez çalışır). Veri gelmezse ESP pinlerini
// kendisi ölçer ve nedenini Serial'e "[mik]" satırlarıyla yazar.
enum MicState : uint8_t {
  MIC_UNKNOWN,      // test henüz bitmedi
  MIC_OK,           // veri geliyor (en az bir mikrofondan)
  MIC_REMAPPED,     // teller başka sırada lehimlenmiş; çalışırken düzeltildi
  MIC_NO_CLOCK,     // ESP saat sinyali üretemiyor (yazılım hatası)
  MIC_LINE_FAULT,   // SCK, WS ya da SD hattı GND'ye / 3V3'e değiyor
  MIC_SILENT,       // saat var, hatlar temiz, mikrofon cevap vermiyor
  MIC_GARBAGE       // veri hattı oynuyor ama ses değil
};
uint8_t micDiag();
const char *micDiagText();
void micRetest();           // öz-testi yeniden çalıştırır (Serial'de "m")
// Mikrofon saatinin hızı (16000 ya da 8000; 8000'de ses yazılımda 16 kHz'e tamamlanır) ve saat
// hatlarının sürüş gücü (0 en zayıf - 3 en güçlü). Saat yeniden başlatılarak uygulanır. Karşılaştırma testi için.
void micSetConfig(int hz, int drive);
int  micClockHz();
int  micGetDrive();
void micAutoTest(bool on);     // false: karşılaştırma testi sürüyor; kesinti uyarıları susar
// Açılıştan beri: 250 ms'lik pencere sayısı, sol / sağ mikrofonun canlı olduğu pencere sayısı, kesinti sayısı.
void micHealth(uint32_t *windows, uint32_t *left, uint32_t *right, uint32_t *outages);
int  micInUse();            // 0 = iki mikrofonun ortalaması, 1 = yalnız sol, 2 = yalnız sağ
// Canlı izleme: hüküm çalışırken de güncellenir (temassız eklem gidip gelebilir).
bool micAlive(int ch);      // 0 = sol, 1 = sağ: son 250 ms'de gerçek mikrofon verisi var mı
int  micChangePct(int ch);  // örneklerin yüzde kaçı bir öncekinden farklı (canlı mikrofonda ~%99)

// Arayüz ve durum raporu için seviyeler (0-32767)
uint16_t micLevel();
uint16_t spkLevel();
uint16_t micLevelLeft();    // son 1 saniyenin ortalaması
uint16_t micLevelRight();
uint32_t spkOverflowCount();
uint32_t spkUnderrunCount();   // çalarken tamponun boşaldığı an sayısı
// ---- Yankı silme ve araya girme (Aşama 3) ----
// Sonuçlar 'a' ölçümünden: üç NLP ayarı (run[i].nlp: 1 güçlü, 0 normal, 3 kapalı).
struct EchoCal {
  struct Run {
    bool ok;
    int mode, nlp, ramKb;
    float outDb, erleDb, residRefDb, cpuMs, cpuMaxMs, chunkMs;
  };
  bool ok;
  int delay;                // hoparlöre yazılan örneğin mikrofona ulaşması (16 kHz örnek)
  float quality;            // çapraz ilinti tepesi / ortalama
  float ambientDb, echoDb;  // hışırtı yokken ve varken mikrofon seviyesi (dBFS)
  Run run[3];               // NLP güçlü, normal, kapalı (yalnızca doğrusal)
  char err[96];
};
bool echoMeasure(EchoCal &r);                       // ~6 sn sürer; oturum kapalıyken, ana döngüden
const char *echoRunName(const EchoCal::Run &run);
void echoSetCalibration(int delaySamples, const float residByNlp[4]);   // açılışta kayıtlı ölçümden ([nlp])
void echoSetNlp(int nlp);                           // 0 normal, 1 güçlü, 3 kapalı; sonraki kurulumda geçerli
int  echoGetNlp();
const char *echoNlpName(int nlp);
int  echoDelaySamples();                            // < 0: ölçülmedi
void micSetEchoCancel(bool on);                     // mikrofon görevinde yankı silmeyi kur / kaldır
bool micEchoActive();
size_t echoStatusText(char *buf, size_t cap);       // açıksa durum satırı eki (sayaçları sıfırlar)
void   spkSetPrebufMs(int ms); // çalmaya başlamadan önce biriktirilecek ses (varsayılan SPK_PREBUF_MS)
void   spkSetBoostDb(float db); // gürlük artırma kazancı (varsayılan SPK_BOOST_DB; oturum sırasında sağlayıcıya göre)
