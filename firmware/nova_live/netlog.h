// Kablosuz erişim: Serial kaydının kopyası + tarayıcıdan izleme/komut + kablosuz yükleme (OTA).
//
//   Panel:           tarayıcıda http://nova.local (ya da ekranda yazan IP), kullanıcı "nova", şifre OTA_PASS:
//                    durum, ayarlar, hafıza, alarmlar, gündem, hava, ücret, canlı kayıt ve komutlar
//   Yükleme 1:       Arduino IDE > Tools > Port > "nova at ..." (ağ portu), Upload; şifre OTA_PASS
//   Yükleme 2:       ağ portu görünmezse: Sketch > Export Compiled Binary, sayfada "Yazılım yükle" ile
//                    build klasöründeki nova_live.ino.bin dosyasını seç (merged olanı değil)
//
// Hepsi secrets.h içinde OTA_PASS tanımlıysa açılır. Kodda Serial.print yerine Log.print kullanılır:
// Log hem Serial'e yazar hem de son 64 KB'ı bellekte tutar (sayfa buradan okur).
//
// Kablosuz çalışırken kart çökerse kablo olmadan sebebini görmek için:
//   - Açılışta yeniden başlama sebebi yazılır; beklenmedik bir sebepse (çökme, bekçi, düşük voltaj)
//     önceki çalışmanın son satırları da yazılır (yeniden başlamaya dayanan küçük bir bellekte tutulur).
//   - Kart art arda 3 kez çökerse "güvenli kip"te açılır: yalnızca WiFi + bu sayfa + kablosuz yükleme.
//   - Kablosuz yüklenen yeni yazılım, WiFi'ye bağlanıp sayfayı açana kadar "deneme"dedir. O sırada
//     kart yeniden başlarsa önyükleyici eski yazılıma döner: bozuk bir yükleme kablo gerektirmez.
#pragma once
#include <Arduino.h>

#ifdef NOVA_HOST_TEST
#define Log Serial                 // masaüstü testinde yalnızca Serial
#else

class TeeLog : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t n) override;
};
extern TeeLog Log;

struct NetOtaHooks {
  void (*start)() = nullptr;              // yükleme başlıyor: oturumu kapat, uyandırmayı durdur
  void (*progress)(int pct) = nullptr;    // 0-100
  void (*end)(bool ok) = nullptr;         // ok: yazıldı, kart birazdan yeniden başlar; değilse eski yazılımla devam
};

// nova.local paneli: /durum (cihazın hali, JSON) ve /arac (ayar, not, alarm; modelin araçlarıyla aynı yol)
struct NetPanelHooks {
  size_t (*durum)(char *out, size_t cap) = nullptr;                                // JSON yazar, uzunluğu döner (0: yazılamadı)
  void (*arac)(const char *ad, const char *argsJson, char *out, size_t cap) = nullptr;
};
void netSetPanel(const NetPanelHooks &h); // netStart'tan önce; verilmezse sayfa "bakım kipi" gösterir

void netBegin();                          // Serial.begin'den hemen sonra: önceki çalışmadan kalanlar + kayıt tamponu
void netBootReport();                     // açılış raporu: yeniden başlama sebebi, çökme kaydı, yazılım durumu
int netCrashStreak();                     // art arda beklenmedik yeniden başlama sayısı (bu açılış dahil)
void netStartedFine();                    // bir süre sorunsuz çalıştı: sayaç sıfırlanır
void netConfirmFirmware();                // WiFi + sayfa ayakta: kablosuz yüklenen yazılım kalıcı olur
void netStart(const NetOtaHooks &hooks);  // WiFi ilk bağlandığında: yükleme ve sayfa
void netLoop();                           // loop() içinden sık çağrılır
int netReadCommand();                     // sayfadan gelen komut harfi; yoksa -1
bool netReadQuestion(char *buf, size_t cap);   // sayfadan yazılan soru; yoksa false
bool netEnabled();                        // OTA_PASS geçerli ve sayfa açık mı

#endif
