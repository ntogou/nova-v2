// Nova V2 — nova_live: konuşma modeline doğrudan bağlanan firmware (Aşama 2)
//
// Bilgisayardaki Python beyni yok. ESP32 WiFi üzerinden OpenAI GPT-Live'a (gpt-live-1) kendisi bağlanır,
// mikrofonu akıtır, gelen sesi çalar. Araya girme ('e') açıkken yankı silmeyle Nova konuşurken de dinler.
// (Gemini Live ve GPT Realtime 2.4.3'e kadar vardı; firmware/arsiv/saglayicilar klasöründeler.)
//
//   "Hey Nova":  uykudayken uyandırır ('n' ile kapalı / normal / hassas / çok hassas).
//   BOOT butonu: uykudaysa uyandırır, Nova konuşuyorsa sözünü keser,
//                dinliyorsa oturumu kapatır.
//   Kablosuz:    http://nova.local (kayıt + komut), Arduino IDE ağ portundan yükleme (netlog.h).
//
// Arduino IDE ayarları (Tools menüsü), nova_v2 ile aynı:
//   Board: ESP32S3 Dev Module        PSRAM: OPI PSRAM
//   Flash Size: 16MB (128Mb)          Partition Scheme: Custom (bu klasördeki partitions.csv)
//   esp32 çekirdeği 3.3.12 ya da yenisi
//   2.7.0'dan beri bölüm tablosu partitions.csv'de: yazılıma 6 MB'lık iki bölüm (eskiden 3 MB). Tabloyu
//   yalnızca USB yüklemesi değiştirir; ilk açılışta notlar eski yerden yeni depoya taşınır.
//   (PSRAM ya da Partition yanlışsa derleme durur ve hangi ayar olduğunu yazar)
// Kablosuz yükleme: Tools > Port > "nova at 192.168.x.x" (ağ portu), Upload, şifre OTA_PASS.
// Kütüphaneler: Adafruit GFX, Adafruit ST7735 and ST7789 (ArduinoJson src klasöründe, kurmak gerekmez)
//
// Dosyalar:
//   nova_live.ino  WiFi, buton, Serial komutları, görevler
//   link.cpp       konuşma akışı (uyku, bağlanma, dinleme, konuşma)
//   proto.h        sağlayıcı arayüzü (link.cpp ile model mesajları arasındaki sınır)
//   gptlive.cpp    OpenAI GPT-Live mesajları (aynı anda dinleyip konuşan model; resample.h: 16 -> 24 kHz)
//   wsclient.cpp   WebSocket istemcisi
//   audio.cpp      I2S, mikrofon, çalma, yankı silme ve yankı ölçümü
//   ui.cpp         ekran, pil, LED (nova_v2 ile aynı)
//   kisilik.h      Nova'nın karakteri
//   wake.cpp       "Hey Nova" uyandırma sözü (ESP-SR WakeNet10; model heynova_model.c içinde)
//   netlog.cpp     kablosuz: http://nova.local kayıt/komut sayfası ve kablosuz yükleme (OTA)
//   araclar.cpp    cihaz araçları (ses, parlaklık, durum, ayar, hafıza, zamanlayıcı): arka model çağırır
//   hafiza.cpp     kalıcı notlar ve zamanlayıcı/alarmlar (flaşın ffat bölümünde)
//   hava.cpp       hava durumu (Open-Meteo), boştayken çekilir
//   notcikar.cpp   oturum sonu not çıkarma: konuşmadan kalıcı notlar (hafızanın emniyet ağı)
//   gundem.cpp     gündem özeti: günde birkaç kez arka modelden (web araması) haber özeti
//   agistek.cpp    cihazın kendi küçük HTTPS istekleri (hava, not çıkarma, gündem)
//   kisisel.h      sahibin kişisel profili (paylaşılmaz; kisisel.example.h'den kopyala; yoksa onsuz derlenir)
//   partitions.csv bölüm tablosu
//   secrets.h      WiFi, OPENAI_API_KEY ve OTA_PASS (paylaşılmaz; secrets.example.h'den kopyala)
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "config.h"
#include "secrets.h"
#include "audio.h"
#include "ui.h"
#include "link.h"
#include "kisilik.h"
// Sahibin kişisel profili: GitHub'a gitmez (.gitignore). Yoksa onsuz derlenir; örnek: kisisel.example.h
#if __has_include("kisisel.h")
#include "kisisel.h"
#else
static const char KISISEL[] = "";
#endif
#include "wake.h"
#include "netlog.h"
#include "hafiza.h"
#include "araclar.h"
#include "hava.h"
#include "notcikar.h"
#include "gundem.h"
#include "psbuf.h"
#include "src/ArduinoJson-v7.4.2.h"

#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 12)
#error "esp32 cekirdegi 3.3.12 ya da yenisi gerekli (Boards Manager)"
#endif
// Arduino IDE, Tools menüsünü zaman zaman varsayılana döndürüyor. Yanlış ayarla derlenen yazılım
// yüklenmesin diye burada durdurulur (kablosuz yüklemede de aynı ayarlar gerekir).
#if !defined(BOARD_HAS_PSRAM) || !defined(CONFIG_SPIRAM_MODE_OCT)
#error "Arduino IDE > Tools > PSRAM: OPI PSRAM secilmeli (ayarlar varsayilana donmus olabilir)"
#endif
// Bölüm tablosu her durumda bu klasördeki partitions.csv'den gelir; menüdeki seçim yalnızca IDE'nin boyut
// sınırını belirler (Custom: sınırı tablo koyar; eski 3 MB seçimi, yazılım 3 MB'ı geçince "too big" der).
#if !defined(ARDUINO_PARTITION_) && !defined(ARDUINO_PARTITION_app3M_fat9M_16MB)
#error "Arduino IDE > Tools > Partition Scheme: Custom secilmeli (tablo: partitions.csv); Flash Size: 16MB"
#endif

// Ücret sayacının kaydı (aşağıda "ücret sayacı"). Sketch'te tanımlanan türler en üstte durmalı: Arduino IDE
// .ino içindeki işlevlerin bildirimlerini ilk işlevden önce kendiliğinden ekler; tür orada bilinmezse derlenmez.
struct UsageTally {
  int32_t day;        // YYYYMMDD; saat henüz alınmadıysa son bilinen gün
  uint32_t daySec;    // o günün ücretlenen saniyesi
  uint32_t totalSec;  // sayaç başından beri
  uint32_t sessions, silent;
  int32_t since;      // sayacın başladığı gün
};

// ---------------------------------------------------------------- TLS bağlantısı
// Sunucu sertifikası çekirdeğin içindeki kök sertifika paketiyle doğrulanır.
class EspTransport : public WsTransport {
 public:
  bool open(const char *host, uint16_t port, uint32_t timeoutMs) override {
    client.stop();
    client.useBuiltinCACertBundle();
    client.setHandshakeTimeout(timeoutMs / 1000 ? timeoutMs / 1000 : 1);
    if (!client.connect(host, port, (int32_t)timeoutMs)) {
      char e[120] = "";
      client.lastError(e, sizeof(e));
      Log.printf("[ağ] TLS bağlantısı kurulamadı: %s (iç RAM boş %u KB, en büyük blok %u KB)\n", e[0] ? e : "(ayrıntı yok)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
      client.stop();
      return false;
    }
    return true;
  }
  void close() override { client.stop(); }
  bool connected() override { return client.connected(); }
  int available() override { return client.available(); }
  int read(uint8_t *buf, size_t n) override {
    int r = client.read(buf, n);
    return r < 0 ? 0 : r;
  }
  bool write(const uint8_t *buf, size_t n) override {
    size_t off = 0;
    uint32_t t0 = millis();
    while (off < n) {
      size_t w = client.write(buf + off, n - off);
      if (w == 0) {
        if (!client.connected() || millis() - t0 > 8000) return false;
        client.available();   // gönderim beklerken gelen TLS parçalarını çek: TCP alım penceresi tıkanmasın
        delay(2);
        continue;
      }
      off += w;
    }
    return true;
  }

 private:
  NetworkClientSecure client;
};

static EspTransport transport;
static Preferences prefs;

static bool wifiUp = false;
static uint32_t lastStatus = 0;
static uint32_t lastWifiKick = 0;
static uint32_t lastRssi = 0;

// ---------------------------------------------------------------- modele verilen bağlam
static void buildSystemText(char *buf, size_t cap) {
  static const char *const GUN[7] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
  char when[96] = "";
  struct tm tm;
  if (getLocalTime(&tm, 0) && tm.tm_year > 120) {
    snprintf(when, sizeof(when), "\nŞu an: %02d.%02d.%04d %s, saat %02d:%02d (Türkiye).",
             tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, GUN[tm.tm_wday % 7], tm.tm_hour, tm.tm_min);
  }
  int n = snprintf(buf, cap, "%s\n%s\n\n%s%s%s%s", KISILIK, GPTLIVE_NOTU, CIHAZ_NOTU, KISISEL[0] ? "\n\n" : "", KISISEL, when);
  if (n < 0 || n >= (int)cap) {
    Log.printf("[bağ] UYARI: kişilik metni %d bayt, sınır %u bayt; sonu kesildi (link.cpp SYS_CAP)\n", n, (unsigned)cap);
    return;
  }
  size_t o = (size_t)n;
  static char *ek = nullptr;                     // 1 KB, PSRAM'de
  const size_t EK = 1024;
  if (!psLazy(ek, EK)) return;
  if (havaYazi(ek, EK, millis())) {
    n = snprintf(buf + o, cap - o, "\n\n%s Hava sorulursa bunu kullan; başka bir yer ya da üç günden sonrası sorulursa arka sisteme sor.", ek);
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
  }
  int64_t t = time(nullptr) > 1700000000 ? (int64_t)time(nullptr) : 0;
  static char *gb = nullptr;                     // gündem bloğu (PSRAM)
  const size_t GB = GUNDEM_CAP + 96;
  if (t && psLazy(gb, GB) && gundemYazi(gb, GB, t, YEREL_SAAT_FARKI)) {
    n = snprintf(buf + o, cap - o, "\n\n%s", gb);
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
  }
  if (t && hafiza::zamanlayicilar(ek, EK, t)) {
    n = snprintf(buf + o, cap - o, "\n\nKurulu zamanlayıcı ve alarmlar: %s.", ek);
    if (n > 0 && (size_t)n < cap - o) o += (size_t)n;
  }
  if (cap - o > 64) {
    buf[o++] = '\n';
    buf[o++] = '\n';
    buf[o] = 0;
    size_t h = hafiza::promptaYaz(buf + o, cap - o);
    if (!h) buf[o - 2] = 0;                    // not yok: boş satırları geri al
  }
}

static uint32_t hookNow() { return (uint32_t)millis(); }
static uint32_t hookRnd() { return esp_random(); }
static void hookIdle() { vTaskDelay(1); }
static void *hookAlloc(size_t n) {
  void *p = psramFound() ? ps_malloc(n) : nullptr;
  return p ? p : malloc(n);
}

// ---------------------------------------------------------------- ücret sayacı (GPT-Live)
// Sunucunun bildirdiği oturum süreleri kartın hafızasında toplanır; yeniden başlatma ve yazılım
// yükleme silmez. Kayıt sayfası kapalıyken geçen oturumlar da böylece görünür. "Konuşmasız" oturum:
// açılmış ama ne söz ne yazılı soru gelmiş (yanlış uyanma ya da vazgeçilmiş). Arka modelin
// (arama) jeton ücreti burada yok; kesin tutar OpenAI panelinde (platform.openai.com/usage).
// (UsageTally dosyanın başında tanımlı: Arduino IDE işlev bildirimlerini en üste ekliyor)

static int32_t todayYmd() {
  struct tm tm;
  if (getLocalTime(&tm, 0) && tm.tm_year > 120) return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
  return 0;
}

static UsageTally usageLoad() {
  UsageTally u = {};
  if (prefs.getBytesLength("usage") == sizeof(u)) prefs.getBytes("usage", &u, sizeof(u));
  return u;
}

static void usagePrint(const UsageTally &u, const char *dayLabel) {
  char day[16] = "?";
  if (u.day) snprintf(day, sizeof(day), "%02d.%02d", (int)(u.day % 100), (int)(u.day / 100 % 100));
  char since[16] = "?";
  if (u.since) snprintf(since, sizeof(since), "%02d.%02d", (int)(u.since % 100), (int)(u.since / 100 % 100));
  Log.printf("[ücret] GPT-Live %s (%s): %.1f dk (~%.2f $) | sayaç başı %s, o günden beri %.1f dk (~%.2f $), %u oturum (konuşmasız: %u). Arka model ayrıca.\n",
             dayLabel, day, u.daySec / 60.0f, u.daySec / 60.0f * GPTLIVE_USD_PER_MIN, since, u.totalSec / 60.0f,
             u.totalSec / 60.0f * GPTLIVE_USD_PER_MIN, (unsigned)u.sessions, (unsigned)u.silent);
}

// Ağ görevinden, oturum kapanınca çağrılır (NVS yazımı iş parçacığı güvenli)
static void hookUsage(float seconds, bool userSpoke) {
  UsageTally u = usageLoad();
  int32_t today = todayYmd();
  if (today && today != u.day) {
    u.day = today;
    u.daySec = 0;
  }
  if (!u.since) u.since = today;
  uint32_t s = (uint32_t)(seconds + 0.5f);
  u.daySec += s;
  u.totalSec += s;
  u.sessions++;
  if (!userSpoke) u.silent++;
  prefs.putBytes("usage", &u, sizeof(u));
  usagePrint(u, "bugün");
}



// ---------------------------------------------------------------- açılış sesi ve test tonları
static const int TONE_N = SPK_RATE / 10;       // 100 ms
static const size_t TONE_BYTES = TONE_N * sizeof(int16_t);
static int16_t *toneBuf = nullptr;            // TONE_N örnek, PSRAM'de (setup alır)
// Gürlük artırma (SPK_BOOST_DB) her sesi yükselttiği için açılış sesi ve amfi testi o kadar
// kısık üretilir: kulağa eskisi gibi gelirler. 'g' melodisi ise bilerek tavanda çalar.
static const float SPK_BOOST_LIN = powf(10.0f, SPK_BOOST_DB / 20.0f);
static const float BOOT_CHIME_AMP = 14000.0f / SPK_BOOST_LIN;
static const float TEST_TONE_AMP = 16000.0f / SPK_BOOST_LIN;

static void bootChime() {
  const float freqs[2] = {880.0f, 1320.0f};
  if (!toneBuf) return;
  for (int k = 0; k < 2; k++) {
    for (int rep = 0; rep < 2; rep++) {        // nota başına 200 ms
      for (int i = 0; i < SPK_RATE / 10; i++) {
        float t = (rep * (SPK_RATE / 10) + i) / (SPK_RATE / 5.0f);
        float env = sinf(PI * t);              // yumuşak giriş-çıkış
        toneBuf[i] = (int16_t)(BOOT_CHIME_AMP * env * sinf(2.0f * PI * freqs[k] * (rep * (SPK_RATE / 10) + i) / SPK_RATE));
      }
      spkWrite((const uint8_t *)toneBuf, TONE_BYTES);
    }
  }
  spkMarkEnd();
}

// Oturum hazır sesi: "şimdi konuşabilirsin" (iki kısa nota, 70'er ms). Ağ görevinden çağrılır;
// iç RAM az olduğu için küçük parçalarla üretilir.
static uint32_t readyChime() {
  int16_t buf[240];                              // 10 ms
  const float freqs[2] = {1047.0f, 1568.0f};
  const int n = SPK_RATE * 7 / 100;
  for (int k = 0; k < 2; k++) {
    for (int i0 = 0; i0 < n; i0 += 240) {
      int m = n - i0 < 240 ? n - i0 : 240;
      for (int j = 0; j < m; j++) {
        int i = i0 + j;
        float env = sinf(PI * i / n);
        buf[j] = (int16_t)(BOOT_CHIME_AMP * 0.6f * env * sinf(2.0f * PI * freqs[k] * i / SPK_RATE));
      }
      spkWrite((const uint8_t *)buf, (size_t)m * 2);
    }
  }
  spkMarkEnd();
  return 140;
}

static void wakeModeCommand() {
  int m = (wakeMode() + 1) % 4;                 // kapalı -> normal -> hassas -> çok hassas -> kapalı
  wakeSetMode(m);
  prefs.putInt("wake", m);
  linkSetWakeEnabled(m != WAKE_OFF);
  linkWakeRearm();
  uiSetBootHint(m == WAKE_OFF);
  Log.printf("[komut] uyandırma sözü: %s%s\n", wakeModeName(m),
                m == WAKE_OFF ? " (BOOT ile uyandır)" : m == WAKE_NORMAL ? " (daha az yanlış uyanma)"
                : m == WAKE_VERY ? " (en çok yakalar; yanlış uyanma artar, 'u' satırındaki konuşmasız oturumlara bak)"
                                 : " (daha çok yakalar)");
}

static uint32_t routeRestoreAt = 0;
static bool audioOk = false;
static volatile bool otaActive = false;       // kablosuz yükleme sürüyor

// Açılış sesi mikrofon öz-testi bittikten sonra çalınır: test, hoparlör sesi mikrofonlara
// dolarken ölçüm yapmasın.
static void pumpBootChime() {
  static bool done = false;
  if (done || !audioOk) return;
  if (micDiag() == MIC_UNKNOWN && millis() < 4000) return;
  done = true;
  if (linkState() == LINK_IDLE) bootChime();
}

static void testTone(bool left, bool right) {
  if (linkState() != LINK_IDLE) { Log.println("[test] önce oturumu kapat (x)"); return; }
  if (!toneBuf) return;
  audioRoute(left, right);
  spkFlush();
  for (int k = 0; k < 10; k++) {               // 1 saniye, 660 Hz
    for (int i = 0; i < SPK_RATE / 10; i++) {
      long n = (long)k * (SPK_RATE / 10) + i;
      toneBuf[i] = (int16_t)(TEST_TONE_AMP * sinf(2.0f * PI * 660.0f * n / SPK_RATE));
    }
    spkWrite((const uint8_t *)toneBuf, TONE_BYTES);
  }
  spkMarkEnd();
  routeRestoreAt = millis() + 1600;
  Log.printf("[test] %s, 1 sn ses (ses seviyesi %%%d)\n",
                left && right ? "iki amfi" : (left ? "yalnız SOL amfi" : "yalnız SAĞ amfi"), spkGetVolume());
}

// 'g': gürlük testi. Cihazın o anki ses ayarında çıkabildiği en yüksek temiz seviyede kısa bir
// melodi çalar (sınırlayıcının tavanında). Konuşma da artık buna yakın seviyede çalınır. Odanın
// öbür ucundan dinleyip donanımın yetip yetmediğine karar vermek için. '+' / '-' ile ses ayarını
// değiştirip yeniden dene: bozulmanın başladığı yer amfinin sınırıdır.
static void loudnessTest() {
  if (linkState() != LINK_IDLE) { Log.println("[test] önce oturumu kapat (x)"); return; }
  static const float NOTES[] = {523.25f, 659.25f, 783.99f, 1046.5f, 783.99f, 659.25f, 587.33f, 698.46f, 880.0f, 1174.7f, 880.0f, 698.46f, 523.25f};
  const int nNotes = sizeof(NOTES) / sizeof(NOTES[0]);
  const int noteLen = SPK_RATE * 4 / 10;         // 400 ms
  if (!toneBuf) return;
  audioRoute(true, true);
  spkFlush();
  float ph = 0;
  int filled = 0;
  for (int k = 0; k < nNotes; k++) {
    float w = 2.0f * PI * NOTES[k] / SPK_RATE;
    for (int i = 0; i < noteLen; i++) {
      float t = (float)i / SPK_RATE;
      float env = (i < 360 ? i / 360.0f : 1.0f) * (0.45f + 0.55f * expf(-t * 6.0f));       // 15 ms giriş, sonra hafif sönüm
      if (i > noteLen - 720) env *= (noteLen - i) / 720.0f;                               // 30 ms çıkış
      ph += w;
      if (ph > 2.0f * PI) ph -= 2.0f * PI;
      float v = sinf(ph) + 0.35f * sinf(2.0f * ph) + 0.15f * sinf(3.0f * ph);
      // Giriş yarım ölçekte: kazançla tavanı aşar, sınırlayıcı tam tavanda tutar.
      toneBuf[filled++] = (int16_t)(16000.0f * env * v / 1.35f);
      if (filled == TONE_N) { spkWrite((const uint8_t *)toneBuf, TONE_BYTES); filled = 0; }
    }
  }
  if (filled) spkWrite((const uint8_t *)toneBuf, (size_t)filled * 2);
  spkMarkEnd();
  routeRestoreAt = millis() + (uint32_t)nNotes * 400 + 800;
  Log.printf("[test] gürlük testi: %.1f sn melodi, bu ses ayarındaki (%%%d) en yüksek temiz seviye. Uzaktan dinle; '+' / '-' ile değiştirip yeniden 'g'.\n",
                nNotes * 0.4f, spkGetVolume());
}

// ---------------------------------------------------------------- mikrofon arıza bulma kipi
// 'c': her yarım saniyede iki mikrofonun canlı durumunu yazar ve karttaki LED'i yakar:
// yeşil = ikisi de var, sarı = biri var, kırmızı = ikisi de yok. Telleri oynatırken
// hangi dokunuşta geldiğini görmek için. 5 dakika sonra kendiliğinden kapanır.
static bool micWatch = false;
static uint32_t micWatchUntil = 0, micWatchLast = 0;

static void micWatchToggle() {
  micWatch = !micWatch;
  if (micWatch) {
    micWatchUntil = millis() + 300000;
    micWatchLast = 0;
    Log.println("[mik] canlı izleme AÇIK (kapatmak için yine 'c'). LED: yeşil = iki mikrofon var, sarı = biri, kırmızı = yok.");
  } else {
    uiLedRelease();
    Log.println("[mik] canlı izleme kapalı");
  }
}

static void pumpMicWatch() {
  static int lastMask = -1;
  static uint8_t lastState = MIC_UNKNOWN;
  uint8_t st = micDiag();
  bool ok = st == MIC_OK || st == MIC_REMAPPED || st == MIC_UNKNOWN;
  linkSetMicOk(ok);
  if (st != lastState) {                       // hüküm değişti: ekrana da yaz
    if (lastState != MIC_UNKNOWN || !ok) uiText("nova", ok ? UI_TR("Mikrofon tamam.", "Mics OK.") : UI_TR("Mikrofonlardan ses gelmiyor.", "No sound from the mics."), true);
    lastState = st;
  }
  if (!micWatch) return;
  uint32_t now = millis();
  if ((int32_t)(now - micWatchUntil) >= 0) { micWatchToggle(); return; }
  if (now - micWatchLast < 500) return;
  micWatchLast = now;
  bool l = micAlive(0), r = micAlive(1);
  int mask = (l ? 1 : 0) | (r ? 2 : 0);
  uiLedOverride(mask == 3 ? 0 : 30, mask == 0 ? 0 : (mask == 3 ? 30 : 18), 0);
  Log.printf("[mik] canlı: SOL %s (%%%d) | SAĞ %s (%%%d)%s\n", l ? "VAR" : "yok", micChangePct(0),
                r ? "VAR" : "yok", micChangePct(1), mask != lastMask && lastMask >= 0 ? "   <-- DEĞİŞTİ" : "");
  lastMask = mask;
}

// ---------------------------------------------------------------- mikrofon sağlığı
// Dakikada bir sağlık satırı: son 60 saniyede mikrofonlar zamanın yüzde kaçında canlıydı. Bir şeyi
// değiştirip (anahtar, tel, ayar) etkisini görmek için: önceki ve sonraki dakikaları karşılaştır.
static uint32_t hpAt = 0, hpWin = 0, hpL = 0, hpR = 0, hpOut = 0;
static bool hpInit = false;

static void pumpMicHealth() {
  if (micDiag() == MIC_UNKNOWN) return;          // açılış testi sürüyor
  uint32_t now = millis();
  if (hpInit && now - hpAt < 60000) return;
  uint32_t w, l, r, o;
  micHealth(&w, &l, &r, &o);
  if (hpInit && w > hpWin) {
    uint32_t dw = w - hpWin;
    Log.printf("[mik] son 60 sn: SOL %%%u | SAĞ %%%u ayakta, %u kesinti (açılıştan beri SOL %%%u | SAĞ %%%u, %u kesinti)\n",
                  (unsigned)((l - hpL) * 100 / dw), (unsigned)((r - hpR) * 100 / dw), (unsigned)(o - hpOut),
                  (unsigned)(w ? l * 100 / w : 0), (unsigned)(w ? r * 100 / w : 0), (unsigned)o);
  }
  hpInit = true;
  hpAt = now;
  hpWin = w; hpL = l; hpR = r; hpOut = o;
}

// ---------------------------------------------------------------- anahtar
static void warnIfKeyMissing() {
  if (linkKeyPresent()) return;
  Log.println("[bağ] UYARI: secrets.h içinde OPENAI_API_KEY doldurulmamış.");
  uiText("nova", UI_TR("secrets.h: OPENAI_API_KEY boş", "secrets.h: OPENAI_API_KEY is empty"), true);
}

// ---------------------------------------------------------------- ses tonu
// 'o' sonraki, 'O' önceki ses. Açık oturum kapanır, yeni sesle bağlanılır ve Nova kendini kısaca tanıtır
// (her deneme birkaç saniyelik bir oturum, ~1 kuruş). Seçim kartta "vglive" adıyla (listedeki sırası) saklanır.
static const char *const VOICES[] = {GPTLIVE_VOICES};
static const int VOICE_N = sizeof(VOICES) / sizeof(VOICES[0]);

static void applyStoredVoice() {
  int i = prefs.getInt("vglive", 0);
  if (i > 0 && i < VOICE_N) linkSetVoice(VOICES[i]);
}

static void voiceCommand(int step) {
  int i = (prefs.getInt("vglive", 0) + step + VOICE_N) % VOICE_N;
  prefs.putInt("vglive", i);
  linkSetVoice(VOICES[i]);                       // ses oturum açılırken seçilir: sonraki konuşmada geçerli
  Log.printf("[komut] ses tonu: %s (%d/%d), bir sonraki konuşmada geçerli. Sonraki / önceki için 'o' / 'O'.\n",
             VOICES[i], i + 1, VOICE_N);
}

// ---------------------------------------------------------------- Serial komutları
// ---------------------------------------------------------------- yankı ölçümü ve araya girme (Aşama 3)
static volatile bool yankiOlcuyor = false;     // ölçüm sürüyor: hava durumu çekilmesin (iç RAM)
static bool echoMeasureCommand() {
  if (linkState() != LINK_IDLE) { Log.println("[yankı] önce oturumu kapat (x)"); return false; }
  struct Bayrak { Bayrak() { yankiOlcuyor = true; } ~Bayrak() { yankiOlcuyor = false; } } bayrak;
  if (!audioOk) { Log.println("[yankı] ses yolu çalışmıyor"); return false; }
  Log.printf("[yankı] ölçüm: 0,5 sn sessizlik, sonra 3 sn hışırtı (ses ayarı %%%d). Sessiz ol, cihaza dokunma.\n", spkGetVolume());
  linkSetHold(true);                            // uyandırma motoru dursun, oturum açılmasın
  delay(400);
  spkSetBoostDb(SPK_BOOST_DB_GPTLIVE);          // oturumdaki gürlükle ölç
  EchoCal c;
  bool ok = echoMeasure(c);
  spkSetBoostDb(SPK_BOOST_DB);
  linkSetHold(false);
  if (!ok) {
    Log.printf("[yankı] ölçüm başarısız: %s\n", c.err);
    return false;
  }
  Log.printf("[yankı] gecikme %.1f ms (güven %.0f) | ortam %.0f dBFS | hışırtının yankısı %.0f dBFS\n", c.delay / 16.0f, c.quality,
             c.ambientDb, c.echoDb);
  float resid[4] = {-20.0f, -20.0f, -20.0f, -20.0f};
  int used = echoGetNlp();
  for (int i = 0; i < 3; i++) {
    const EchoCal::Run &r = c.run[i];
    if (!r.ok) { Log.printf("[yankı] %s: kurulamadı\n", echoRunName(r)); continue; }
    resid[r.nlp & 3] = r.residRefDb;
    Log.printf("[yankı] %s%s: artık %.0f dBFS, bastırma %.0f dB, işlem %.1f ms (en çok %.1f) / %.0f ms, iç RAM %d KB\n",
               echoRunName(r), r.nlp == used ? " (kullanılan)" : "", r.outDb, r.erleDb, r.cpuMs, r.cpuMaxMs, r.chunkMs, r.ramKb);
    if (r.nlp == used)
      Log.printf("[yankı]     Nova konuşurken sesinin modele geçmesi için mikrofonda kabaca %.0f dBFS'in üstünde olması gerekir (artık + %.0f dB pay)\n",
                 r.outDb + ECHO_GATE_MARGIN_DB, (double)ECHO_GATE_MARGIN_DB);
  }
  echoSetCalibration(c.delay, resid);
  prefs.putInt("echoD", c.delay);
  prefs.putBytes("echoRs", resid, sizeof(resid));
  prefs.putInt("echoV", 2);                     // 2: yankı silme girişi ECHO_IN_HEADROOM_DB kısılmış ölçekte
  Log.println("[yankı] ölçüm kartta saklandı. Araya girme için 'e' (sayfada \"Araya girme\").");
  return true;
}

static void bargeInCommand() {
  bool on = !linkBargeIn();
  if (on && echoDelaySamples() < 0) {
    Log.println("[komut] araya girme için önce yankı ölçülüyor");
    if (!echoMeasureCommand()) return;
  }
  linkSetBargeIn(on);
  prefs.putBool("barge", on);
  Log.printf("[komut] araya girme %s%s\n", on ? "AÇIK" : "kapalı",
             on ? ": sonraki oturumdan itibaren Nova konuşurken de dinler; konuşunca susar" : " (Nova konuşurken mikrofon kapalı)");
}

// 'E': yankı silmenin NLP ayarı sırayla normal -> güçlü -> kapalı (sonraki oturumda geçerli)
// (2.6.1'den beri sayfada yok ve kaydedilmez: hep ECHO_NLP_DEFAULT. Yalnızca Serial'den deneme için.)
static void echoNlpCommand() {
  int cur = echoGetNlp();
  int next = cur == 0 ? 1 : cur == 1 ? 3 : 0;
  echoSetNlp(next);
  Log.printf("[komut] yankı silme NLP: %s (sonraki oturumda geçerli; sırayla normal -> güçlü -> kapalı)\n", echoNlpName(next));
}

// Hava durumu boştayken ağ görevinde çekilir. Şifreli bağlantı iç RAM ister: uyandırma motoru birkaç saniye kalkar.
static uint32_t havaSonDeneme = 0;
static uint32_t notSonDeneme = 0;
static volatile uint32_t oturumBittiAt = 0;      // ana döngü yazar: son oturumun kapandığı an

// Cihazın kendi şifreli bağlantısı için hazırlık: uyandırma motoru kalkar, WiFi uyanır. false: oturum açıldı.
static bool agIsiBasla() {
  linkSetNetBusy(true);
  linkLoop();                                    // uyandırma motoru kalksın (iç RAM bağlantıya kalsın)
  if (linkState() != LINK_IDLE) { linkSetNetBusy(false); return false; }
  wifiUyandir();
  return true;
}

// Oturum sonu not çıkarma (hafızanın emniyet ağı): oturum kapandıktan 2 sn sonra, boştayken
static bool notAdimi() {
  uint32_t now = millis();
  if (!notBekliyor() || !wifiUp || otaActive || yankiOlcuyor || linkState() != LINK_IDLE) return false;
  if (now - oturumBittiAt < 2000 || (notSonDeneme && now - notSonDeneme < 60000)) return false;
  notSonDeneme = now;
  if (!agIsiBasla()) return true;
  char rapor[700];
  bool bitti = notCikar(todayYmd(), rapor, sizeof(rapor));
  linkSetNetBusy(false);
  if (bitti) notSonDeneme = 0;
  Log.printf("[hafıza] konuşmadan: %s (%u ms)%s\n", rapor, (unsigned)(millis() - now), bitti ? "" : "; bir dakika sonra yeniden denenecek");
  return true;
}

static void havaAdimi() {
#if HAVA_ACIK
  uint32_t now = millis();
  static uint32_t ekranAt = 0;                  // uyku ekranındaki kısa hava (özet eskiyince silinir)
  if (!ekranAt || now - ekranAt > 30000) {
    ekranAt = now | 1;
    char k[40];
    uiSetHava(havaKisa(k, sizeof(k), now) ? k : "");
  }
  if (!wifiUp || otaActive || yankiOlcuyor || linkState() != LINK_IDLE || now < 15000) return;
  if (havaSonDeneme && now - havaSonDeneme < HAVA_TEKRAR_MS) return;
  uint32_t al = havaAlindiMs();
  if (al && now - al < HAVA_YENILE_MS) return;
  havaSonDeneme = now;
  if (!agIsiBasla()) return;
  char err[80];
  bool ok = havaCek(err, sizeof(err));
  linkSetNetBusy(false);
  if (ok) {
    char b[700];
    havaYazi(b, sizeof(b), millis());
    char k[40];
    if (havaKisa(k, sizeof(k), millis())) uiSetHava(k);
    Log.printf("[hava] %s (%u ms)\n", b, (unsigned)(millis() - now));
  } else {
    Log.printf("[hava] alınamadı: %s (%u dk sonra yeniden denenecek)\n", err, (unsigned)(HAVA_TEKRAR_MS / 60000));
  }
#endif
}

// Gündem özeti: GUNDEM_SAATLER'deki her saatten sonra bir kez. İstek arka planda çalışır; kart bırakıp yoklar,
// her bağlantıda uyandırma motoru yalnızca 1-2 sn kalkar.
static uint32_t gundemSonDeneme = 0;
static int gundemHata = 0;
static int64_t gundemAtla = 0;                   // başarısız saat: bir sonraki gündem saatine kadar denenmez
static volatile bool kaydetGundem = false;       // ana döngü NVS'ye yazar

static void gundemTarih(int64_t t, char *out, size_t cap) {
  static const char *const GUN[7] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
  int y, a, g, s, d, hg;
  hafiza::yerelTakvim(t, &y, &a, &g, &s, &d, &hg);
  snprintf(out, cap, "%02d.%02d.%04d %s, saat %02d:%02d", g, a, y, GUN[hg % 7], s, d);
}

static bool gundemAdimi() {
#if GUNDEM_ACIK
  uint32_t now = millis();
  if (!wifiUp || otaActive || yankiOlcuyor || linkState() != LINK_IDLE || now < 20000) return false;
  int64_t t = (int64_t)time(nullptr);
  if (t < 1700000000) return false;
  char hata[240] = "";
  if (gundemBekliyor()) {
    if (now - gundemSonDeneme < GUNDEM_YOKLA_MS) return false;
    gundemSonDeneme = now;
    if (!agIsiBasla()) return true;
    int r = gundemYokla(t, hata, sizeof(hata));
    linkSetNetBusy(false);
    if (r > 0) {
      gundemHata = 0;
      kaydetGundem = true;
      Log.printf("[gündem] alındı (%u bayt):\n%s\n", (unsigned)strlen(gundemMetni()), gundemMetni());
    } else if (r < 0) {
      gundemAtla = t;
      Log.printf("[gündem] alınamadı: %s (bir sonraki gündem saatinde yeniden)\n", hata);
    }
    return true;
  }
  if (gundemSonDeneme && now - gundemSonDeneme < GUNDEM_TEKRAR_MS) return false;
  int64_t son = gundemVakti() > gundemAtla ? gundemVakti() : gundemAtla;
  if (!gundemZamani(son, t, YEREL_SAAT_FARKI)) return false;
  gundemSonDeneme = now;
  if (!agIsiBasla()) return true;
  char tarih[64];
  gundemTarih(t, tarih, sizeof(tarih));
  int r = gundemBaslat(t, tarih, hata, sizeof(hata));
  linkSetNetBusy(false);
  if (r > 0) {
    gundemSonDeneme = now;                       // ilk yoklama GUNDEM_YOKLA_MS sonra
    Log.printf("[gündem] istendi (%u ms); arka planda hazırlanıyor\n", (unsigned)(millis() - now));
  } else if (r < 0 || ++gundemHata >= 3) {
    gundemHata = 0;
    gundemAtla = t;
    Log.printf("[gündem] istenemedi: %s (bir sonraki gündem saatinde yeniden)\n", hata);
  } else {
    Log.printf("[gündem] istenemedi: %s (%u dk sonra yeniden)\n", hata, (unsigned)(GUNDEM_TEKRAR_MS / 60000));
  }
  return true;
#else
  return false;
#endif
}

static void netTask(void *) {
  for (;;) {
    linkLoop();
    if (!notAdimi() && !gundemAdimi()) havaAdimi();
    vTaskDelay(pdMS_TO_TICKS(linkState() == LINK_IDLE ? 20 : (transport.available() > 0 ? 1 : 2)));
  }
}
// ---------------------------------------------------------------- cihaz araçları (Aşama 5)
// Arka modelin çağırdığı araçlar ağ görevinde çalışır. Kalıcı kayıt (prefs) ve ekran ana döngüde yapılır.
static volatile bool kaydetSes = false;
static volatile int kaydetAraya = -1, kaydetUyandirma = -1, kaydetSesTonu = -1;
static volatile int istenenParlaklik = -1;        // araç parlaklık istedi: ana döngü uygular ve saklar
static int normalParlaklik = DEFAULT_BRIGHTNESS;  // kullanıcının seçtiği parlaklık (boşta kısılınca buna döner)

static int64_t simdiEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (int64_t)t : 0;
}
static int aracSes() { return spkGetVolume(); }
static void aracSesAyarla(int v) { spkSetVolume(v); kaydetSes = true; }
static int aracParlaklik() { return normalParlaklik; }
static void aracParlaklikAyarla(int v) { istenenParlaklik = v; }
static uint32_t aracMs() { return millis(); }

static bool aracAyar(const char *ad, const char *deger, char *msg, size_t cap) {
  char d[48];
  hafiza::katla(deger, d, sizeof(d));
  if (!strcmp(ad, "barge_in")) {
    bool on = !strcmp(d, "on") || !strcmp(d, "acik") || !strcmp(d, "true") || !strcmp(d, "ac") || !strcmp(d, "evet");
    bool off = !strcmp(d, "off") || !strcmp(d, "kapali") || !strcmp(d, "false") || !strcmp(d, "kapat") || !strcmp(d, "hayir");
    if (!on && !off) { snprintf(msg, cap, "Hata: değer on ya da off olmalı."); return false; }
    if (on && echoDelaySamples() < 0) {
      snprintf(msg, cap, "Araya girme için önce yankı ölçümü gerekiyor: kullanıcı oturum kapalıyken cihazın sayfasından \"Yankı ölç\"e basmalı.");
      return false;
    }
    linkSetBargeIn(on);
    kaydetAraya = on ? 1 : 0;
    snprintf(msg, cap, "Araya girme %s; bir sonraki konuşmadan itibaren geçerli.", on ? "açıldı" : "kapatıldı");
    return true;
  }
  if (!strcmp(ad, "wake_sensitivity")) {
    int m = !strcmp(d, "off") || !strcmp(d, "kapali") ? WAKE_OFF
            : !strcmp(d, "normal") ? WAKE_NORMAL
            : !strcmp(d, "sensitive") || !strcmp(d, "hassas") ? WAKE_SENSITIVE
            : !strcmp(d, "very sensitive") || !strcmp(d, "cok hassas") ? WAKE_VERY : -1;
    if (m < 0) { snprintf(msg, cap, "Hata: değer off, normal, sensitive ya da very_sensitive olmalı."); return false; }
    wakeSetMode(m);
    linkSetWakeEnabled(m != WAKE_OFF);
    linkWakeRearm();
    kaydetUyandirma = m;
    snprintf(msg, cap, "Uyandırma sözü: %s.%s", wakeModeName(m), m == WAKE_OFF ? " Artık \"Hey Nova\" ile uyanmaz; BOOT düğmesiyle uyanır." : "");
    return true;
  }
  if (!strcmp(ad, "voice")) {
    int i = -1;
    char k[24];
    for (int j = 0; j < VOICE_N && i < 0; j++) { hafiza::katla(VOICES[j], k, sizeof(k)); if (!strcmp(k, d)) i = j; }
    if (i < 0) { snprintf(msg, cap, "Hata: '%s' diye bir ses yok. Bazıları: marin, cedar, gleam, coral, sage, verse, ballad.", deger); return false; }
    linkSetVoice(VOICES[i]);
    kaydetSesTonu = i;
    snprintf(msg, cap, "Konuşma sesi %s yapıldı; bir sonraki konuşmada geçerli olacak.", VOICES[i]);
    return true;
  }
  snprintf(msg, cap, "Hata: '%s' diye bir ayar yok.", ad);
  return false;
}

static void aracDurum(char *out, size_t cap) {
  struct tm tm;
  char saat[16] = "bilinmiyor";
  if (getLocalTime(&tm, 0) && tm.tm_year > 120) snprintf(saat, sizeof(saat), "%02d:%02d", tm.tm_hour, tm.tm_min);
  int rssi = WiFi.RSSI();
  const char *wq = !wifiUp ? "bağlı değil" : rssi > -60 ? "güçlü" : rssi > -72 ? "orta" : "zayıf";
  int pct = batPercent();
  char pil[40];
  if (pct >= 0) snprintf(pil, sizeof(pil), "%%%d (%.2f V)", pct, batVolts());
  else snprintf(pil, sizeof(pil), "ölçülemiyor");
  snprintf(out, cap, "Pil %s; WiFi %s (%d dBm); ses %%%d; ekran parlaklığı %%%d; uyandırma sözü: %s; araya girme: %s; "
           "konuşma sesi: %s; saat %s; kurulu zamanlayıcı/alarm: %d; hafızada %d not; yazılım %s.",
           pil, wq, rssi, spkGetVolume(), normalParlaklik, wakeModeName(wakeMode()), linkBargeIn() ? "açık" : "kapalı",
           linkVoiceName(), saat, hafiza::zamanlayiciSayisi(), hafiza::notSayisi(), FW_VERSION);
}

// ---------------------------------------------------------------- nova.local paneli
// Sayfanın /durum isteği: cihazın o anki hali tek JSON'da. Büyük olduğu için belleği PSRAM'den alır.
struct PsJsonAlloc : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return psAlloc(n); }
  void deallocate(void *p) override { free(p); }
  void *reallocate(void *p, size_t n) override {
    void *q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};

static void panelNot(void *c, int id, int32_t ymd, char kaynak, const char *metin) {
  JsonObject o = ((JsonArray *)c)->add<JsonObject>();
  o["id"] = id;
  char t[8] = "";
  if (ymd > 0) snprintf(t, sizeof(t), "%02d.%02d", (int)(ymd % 100), (int)(ymd / 100 % 100));
  o["t"] = (const char *)t;
  char k[2] = {kaynak, 0};
  o["k"] = (const char *)k;
  o["m"] = metin;
}

static void panelZaman(void *c, int id, char tur, const char *etiket, int64_t vakit) {
  JsonObject o = ((JsonArray *)c)->add<JsonObject>();
  int64_t simdi = simdiEpoch();
  o["id"] = id;
  o["tur"] = tur == hafiza::ALARM ? "alarm" : "zamanlayıcı";
  o["e"] = etiket;
  char s[24];
  hafiza::saatYaz(vakit, simdi, s, sizeof(s));
  o["s"] = (const char *)s;
  o["sn"] = simdi ? (long)(vakit - simdi) : -1;
}

static size_t panelDurum(char *out, size_t cap) {
  static PsJsonAlloc al;
  JsonDocument d(&al);
  uint32_t now = millis();
  d["v"] = FW_VERSION;
  d["dk"] = now / 60000;
  d["durum"] = otaActive ? "güncelleniyor" : !wifiUp ? "wifi" : linkStateName();
  uint32_t arka = linkBackendSince();
  d["arka"] = arka ? (int)((now - arka) / 1000) : -1;
  d["pil"] = batPercent();
  d["pilV"] = roundf(batVolts() * 100) / 100;
  d["rssi"] = wifiUp ? WiFi.RSSI() : -100;
  int64_t t = simdiEpoch();
  if (t) {
    int y, a, g, sa, dk, hg;
    hafiza::yerelTakvim(t, &y, &a, &g, &sa, &dk, &hg);
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", sa, dk);
    d["saat"] = (const char *)b;
  }
  d["ses"] = spkGetVolume();
  d["parlak"] = normalParlaklik;
  d["uyan"] = wakeMode();
  d["araya"] = linkBargeIn();
  d["yankiOlcu"] = echoDelaySamples() >= 0;
  d["sesTonu"] = linkVoiceName();
  d["anahtar"] = linkKeyPresent();
  JsonArray sv = d["sesler"].to<JsonArray>();
  for (int i = 0; i < VOICE_N; i++) sv.add(VOICES[i]);
  JsonArray na = d["notlar"].to<JsonArray>();
  hafiza::notlariGez(panelNot, &na);
  d["notSinir"] = HAFIZA_MAX_NOT;
  JsonArray za = d["zaman"].to<JsonArray>();
  hafiza::zamanlariGez(panelZaman, &za);
  char *b = (char *)psAlloc(1800);
  if (b) {
    if (gundemVakti() && *gundemMetni()) {
      JsonObject go = d["gundem"].to<JsonObject>();
      int64_t gv = gundemVakti();
      hafiza::saatYaz(gv, t ? t : gv, b, 64);
      go["t"] = (const char *)b;
      go["m"] = gundemMetni();
    }
    if (havaYazi(b, 1800, now)) d["hava"] = (const char *)b;
    free(b);
  }
  UsageTally u = usageLoad();
  JsonObject uo = d["ucret"].to<JsonObject>();
  uo["gunSn"] = u.day && u.day == todayYmd() ? u.daySec : 0;
  uo["topSn"] = u.totalSec;
  uo["oturum"] = u.sessions;
  uo["bas"] = u.since;
  uo["dkUsd"] = GPTLIVE_USD_PER_MIN;
  JsonObject co = d["cihaz"].to<JsonObject>();
  co["mik"] = micDiagText();
  co["icRam"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
  co["blok"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024;
  co["psram"] = ESP.getFreePsram() / 1024;
  char depo[96];
  if (hafiza::flasDurum(depo, sizeof(depo))) co["depo"] = (const char *)depo;
  char ek[120];
  if (uiStatsText(ek, sizeof(ek))) co["ekran"] = (const char *)ek;
  size_t n = serializeJson(d, out, cap);
  return n < cap ? n : 0;                       // sığmadıysa yarım JSON yollanmaz
}

// Sayfadaki ayar, not ve alarm düğmeleri modelin kullandığı araçlarla aynı yoldan geçer
static void panelArac(const char *ad, const char *args, char *out, size_t cap) {
  static const char *const IZINLI[] = {"set_volume", "set_brightness", "change_setting", "remember", "forget",
                                       "set_timer", "set_alarm", "cancel_timer"};
  bool izin = false;
  for (auto a : IZINLI) izin |= !strcmp(ad, a);
  if (!izin) { snprintf(out, cap, "Hata: '%s' sayfadan çağrılamaz.", ad); return; }
  aracCalistir(ad, args, out, cap);
  Log.printf("[sayfa] %s %s -> %s\n", ad, args, out);
}

static void hookTool(const char *name, const char *args, char *out, size_t cap) {
  const char *etiket = aracEtiketi(name);
  if (*etiket) uiSetEtkinlik(etiket);
  aracCalistir(name, args, out, cap);
}
static void hookLine(bool user, const char *text) { notKonusmaEkle(user, text); }

// Oturum kapanınca konuşma not çıkarmaya sıraya alınır
static void pumpOturumSonu() {
  static LinkState onceki = LINK_IDLE;
  LinkState s = linkState();
  if (onceki != LINK_IDLE && s == LINK_IDLE) {
    oturumBittiAt = millis();
    if (notOturumBitti()) Log.println("[hafıza] konuşma bitti; kalıcı notlar birkaç saniye içinde çıkarılacak");
  }
  onceki = s;
}

// Yerel yedek: "sesi kıs / aç / yüzde 40 yap" modeli beklemeden uygulanır
static void hookHeard(const char *t) {
  int v = sesliSesKomutu(t, spkGetVolume());
  if (v < 0) return;
  spkSetVolume(v);
  aracYerelSesYapildi(v);
  kaydetSes = true;
  Log.printf("[komut] (sesle) ses seviyesi %%%d\n", v);
}

static void buildBackendText(char *buf, size_t cap) {
  static const char *const GUN[7] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
  char when[96] = "";
  struct tm tm;
  if (getLocalTime(&tm, 0) && tm.tm_year > 120)
    snprintf(when, sizeof(when), "Şu an: %02d.%02d.%04d %s, saat %02d:%02d (Türkiye saati). ", tm.tm_mday, tm.tm_mon + 1,
             tm.tm_year + 1900, GUN[tm.tm_wday % 7], tm.tm_hour, tm.tm_min);
  snprintf(buf, cap,
           "%s%s\nCihaz araçları: Kullanıcı ses, ekran parlaklığı, pil ya da cihaz durumu, bir ayar, zamanlayıcı, alarm, "
           "hatırlatıcı ya da hafıza ile ilgili bir şey isterse uygun aracı çağır ve sonucu kısa, doğal bir cümleyle bildir. "
           "Araç hata verirse sebebini kısaca söyle; yapılmış gibi söyleme. Hafıza notunu kullanıcı hakkında, üçüncü şahısla, "
           "tek kısa cümle yaz (ör. 'Kullanıcının kız kardeşinin adı Ela.'). Alarm saatini 24 saat biçiminde ver; kullanıcı "
           "gün söylemediyse date verme. Göreli süreleri (yarım saat, bir buçuk saat) dakikaya çevir.",
           when, GPTLIVE_BACKEND_NOTE);
}

// ---------------------------------------------------------------- zamanlayıcı ve alarm çalınca
static uint32_t notifyAt = 0;
static char notifyText[240];
static void bostaUyan();

// Üç kez iki ton (880 ve 1320 Hz, 150'şer ms) ve 300 ms ara: yaklaşık 2 sn
static void alarmZili() {
  if (!toneBuf) return;
  const int nota = SPK_RATE * 15 / 100, ara = SPK_RATE * 3 / 10;
  const float fr[2] = {880.0f, 1320.0f};
  int filled = 0;
  auto push = [&](int16_t v) {
    toneBuf[filled++] = v;
    if (filled == TONE_N) { spkWrite((const uint8_t *)toneBuf, TONE_BYTES); filled = 0; }
  };
  for (int r = 0; r < 3; r++) {
    for (int k = 0; k < 2; k++)
      for (int i = 0; i < nota; i++) {
        float env = sinf(PI * i / nota);
        push((int16_t)(BOOT_CHIME_AMP * env * sinf(2.0f * PI * fr[k] * i / SPK_RATE)));
      }
    for (int i = 0; i < ara; i++) push(0);
  }
  if (filled) spkWrite((const uint8_t *)toneBuf, (size_t)filled * 2);
  spkMarkEnd();
}

static void pumpZamanlayici() {
  static uint32_t son = 0;
  uint32_t now = millis();
  if (notifyAt && (int32_t)(now - notifyAt) >= 0) { notifyAt = 0; linkNotify(notifyText); }
  if (now - son < 1000 || notifyAt) return;
  son = now;
  int64_t t = simdiEpoch();
  hafiza::Calan c;
  if (!t || !hafiza::calanVar(t, &c)) return;
  bool alarm = c.tur == hafiza::ALARM;
  if (c.kacirildi) {
    Log.printf("[zaman] cihaz kapalıyken geçti, çalınmadı: %s (%s)\n", c.etiket, alarm ? "alarm" : "zamanlayıcı");
    return;
  }
  Log.printf("[zaman] %s çaldı: %s\n", alarm ? "alarm" : "zamanlayıcı", c.etiket);
  snprintf(notifyText, sizeof(notifyText),
           "Cihazdan bildirim: kullanıcının kurduğu %s %s (etiket: %s). Bunu kullanıcıya hemen, tek kısa cümleyle söyle.",
           alarm ? "alarm" : "zamanlayıcının süresi", alarm ? "çaldı" : "doldu", c.etiket);
  char ekran[96];
  snprintf(ekran, sizeof(ekran), "%s: %s", alarm ? "Alarm" : UI_TR("Zamanlayıcı", "Timer"), c.etiket);
  uiText("nova", ekran, true);
  bostaUyan();
  if (linkState() == LINK_IDLE && audioOk && !otaActive) {
    alarmZili();
    notifyAt = now + 2300;                      // zil bitince Nova söyler
  } else {
    linkNotify(notifyText);                     // oturum açık: zilsiz, Nova söyler
  }
}

// ---------------------------------------------------------------- boşta güç: ekran kısılır, WiFi tasarrufa geçer
static bool kisik = false;
static volatile bool wifiUyuyor = false;
static uint32_t bostaBasi = 0;
static void bostaUyan() {
  bostaBasi = millis();
  if (!kisik) return;
  kisik = false;
  uiSetBrightness(normalParlaklik);
#if BOSTA_WIFI_UYKU
  WiFi.setSleep(false);
#endif
}
static void pumpBostaGuc() {
  uint32_t now = millis();
  int ip = istenenParlaklik;
  if (ip >= 0) {                                // araç parlaklık istedi
    istenenParlaklik = -1;
    normalParlaklik = ip;
    prefs.putInt("bri", ip);
    kisik = false;
    uiSetBrightness(ip);
  }
  bool bos = linkState() == LINK_IDLE && !otaActive && wifiUp && !notifyAt;
  if (!bos) { bostaUyan(); return; }
  if (BOSTA_KIS_MS == 0 || kisik || now - bostaBasi < (uint32_t)BOSTA_KIS_MS) return;
  kisik = true;
  uiSetBrightness(BOSTA_PARLAKLIK);
#if BOSTA_WIFI_UYKU
  WiFi.setSleep(true);
  wifiUyuyor = true;
#endif
}

// ---------------------------------------------------------------- kalıcı kayıt (ana döngüde)
static void pumpKayit() {
  if (kaydetSes) { kaydetSes = false; prefs.putInt("vol", spkGetVolume()); }
  int v;
  if ((v = kaydetAraya) >= 0) { kaydetAraya = -1; prefs.putBool("barge", v == 1); }
  if ((v = kaydetUyandirma) >= 0) { kaydetUyandirma = -1; prefs.putInt("wake", v); }
  if ((v = kaydetSesTonu) >= 0) { kaydetSesTonu = -1; prefs.putInt("vglive", v); }
  if (kaydetGundem) {                            // son gündem: yeniden başlayınca boşuna yeniden çekilmesin
    kaydetGundem = false;
    prefs.putString("gundem", gundemMetni());
    prefs.putLong64("gundemT", gundemVakti());
  }
  // Hafıza flaşa yalnızca boştayken yazılır (yazarken önbellek kapanır; oturumda ses takılmasın)
  static uint32_t bosSince = 0, sonHata = 0;
  uint32_t now = millis();
  if (linkState() != LINK_IDLE || notifyAt) { bosSince = now; return; }
  if (!hafiza::degisti() || now - bosSince < 3000 || (sonHata && now - sonHata < 60000)) return;
  if (hafiza::kaydet()) {
    sonHata = 0;
    Log.printf("[hafıza] flaşa yazıldı (%d not, %d zamanlayıcı)\n", hafiza::notSayisi(), hafiza::zamanlayiciSayisi());
  } else {
    sonHata = now;
    Log.println("[hafıza] HATA: flaşa yazılamadı, bir dakika sonra yeniden denenecek");
  }
}

static char *yaziTamponu() {                    // listeler için 6 KB, PSRAM'de
  static char *b = nullptr;
  return psLazy(b, 6144);
}
static void printMemory() {
  char *b = yaziTamponu();
  if (!b) return;
  if (hafiza::listele(b, 6144)) Log.print(b);
  char z[700];
  int64_t t = simdiEpoch();
  if (t && hafiza::zamanlayicilar(z, sizeof(z), t)) Log.printf("[hafıza] zamanlayıcılar: %s (iptal: /iptal 2 ya da /iptal hepsi)\n", z);
  else Log.println("[hafıza] kurulu zamanlayıcı yok");
}

// Sayfadaki yazı kutusundan "/" ile başlayan yerel komutlar
static bool slashCommand(const char *q) {
  if (q[0] != '/') return false;
  char *msg = yaziTamponu();
  if (!msg) return true;
  if (!strncmp(q, "/unut ", 6)) {
    hafiza::unut(q + 6, msg, 6144);
    Log.printf("[hafıza] %s\n", msg);
  } else if (!strncmp(q, "/iptal ", 7)) {
    hafiza::iptal(q + 7, msg, 6144);
    Log.printf("[hafıza] %s\n", msg);
  } else {
    printMemory();
  }
  return true;
}

static void printHelp() {
  Log.println("[komut] w = uyandır, x = oturumu kapat, b = BOOT butonu gibi (uyandır / sözünü kes / kapat)");
  Log.println("[komut] s = yazılı deneme sorusu gönder (mikrofonsuz), v = ayrıntılı kayıt aç/kapa");
  Log.println("[komut] n = \"Hey Nova\" uyandırma sözü: kapalı / normal / hassas / çok hassas (sırayla)");
  Log.println("[komut] o / O = sonraki / önceki konuşma sesi (bir sonraki konuşmada geçerli); sesle de değişir: \"sesini cedar yap\"");
  Log.println("[komut] sesle kapatma: \"görüşürüz\", \"hoşça kal\", \"oturumu bitir\" gibi bir söz; Nova cevabını bitirince oturum kapanır");
  Log.println("[komut] u = GPT-Live ücret sayacı (bugün ve sayaç başından beri; kartın hafızasında tutulur)");
  Log.println("[komut] a = yankı ölçümü (buluta bağlanmaz, 3 sn hışırtı çalar), e = araya girme aç/kapa");
  Log.println("[komut] H = hafıza ve zamanlayıcılar; sayfada yazıyla: /unut 3, /unut hepsi, /iptal 2");
  Log.println("[komut] + / - = ses, t = iki amfi, l = sol, r = sağ, m = mikrofon testi, ? = bu liste");
  Log.println("[komut] g = gürlük testi: en yüksek temiz seviyede kısa melodi (uzaktan dinlemek için)");
  Log.println("[komut] c = mikrofonları canlı izle (arıza bulma: LED yeşil = var, kırmızı = yok)");
  Log.println("[komut] R = yeniden başlat (büyük R)");
}

static void handleCommand(char c) {
  if (otaActive) return;                         // yükleme sürüyor
  switch (c) {
    case 'R':
      Log.println("[komut] yeniden başlatılıyor");
      hafiza::kaydet();
      linkSleep();
      for (int i = 0; i < 200 && linkState() != LINK_IDLE; i++) delay(10);
      delay(300);
      ESP.restart();
      break;
    case 'w': linkWake(); break;
    case 'x': linkSleep(); break;
    case 'b': linkButton(); break;
    case 's': linkSay("Merhaba Nova, kendini bir cümleyle tanıt."); break;
    case 'n': wakeModeCommand(); break;
    case 'a': echoMeasureCommand(); break;
    case 'e': bargeInCommand(); break;
    case 'E': echoNlpCommand(); break;
    case 'H': printMemory(); break;
    case 'u': {
      UsageTally u = usageLoad();
      int32_t today = todayYmd();
      if (today && today != u.day) { u.day = today; u.daySec = 0; }     // bugün henüz oturum yok
      usagePrint(u, "bugün");
      break;
    }
    case 'o': voiceCommand(+1); break;
    case 'O': voiceCommand(-1); break;
    case 'v':
      linkSetVerbose(!linkVerbose());
      Log.printf("[komut] ayrıntılı kayıt %s\n", linkVerbose() ? "açık" : "kapalı");
      break;
    case 't': testTone(true, true); break;
    case 'g': loudnessTest(); break;
    case 'l': testTone(true, false); break;
    case 'r': testTone(false, true); break;
    case 'm': micRetest(); break;
    case 'c': micWatchToggle(); break;
    case '+':
    case '-': {
      int v = constrain(spkGetVolume() + (c == '+' ? 10 : -10), 0, 100);
      spkSetVolume(v);
      prefs.putInt("vol", v);
      Log.printf("[komut] ses seviyesi %%%d\n", v);
      break;
    }
    case '?': printHelp(); break;
    default: break;
  }
}

static void serialCommands() {
  while (Serial.available()) handleCommand((char)Serial.read());
  for (int c; (c = netReadCommand()) >= 0;) handleCommand((char)c);
  static char q[200];
  if (netReadQuestion(q, sizeof(q)) && !otaActive && !slashCommand(q)) {
    Log.printf("[komut] yazılı soru: %s\n", q);
    linkSay(q);
  }
  if (routeRestoreAt && millis() > routeRestoreAt) {
    routeRestoreAt = 0;
    audioRoute(true, true);
  }
}

// ---------------------------------------------------------------- kablosuz yükleme
// Yükleme loop görevinde, bitene kadar (yarım dakika kadar) sürer. Bu sırada oturum kapalı,
// uyandırma ve bağlanma durur, ekranda ilerleme çubuğu görünür. Başarılıysa kart kendiliğinden yeniden başlar.
static void otaStarting() {
  otaActive = true;
  hafiza::kaydet();
  linkSetHold(true);
  spkFlush();
  uiSetOta(0);
  uiSetState(UI_OTA);
}

static void otaProgress(int pct) { uiSetOta(pct); }

static void otaFinished(bool ok) {
  if (ok) return;                                // kart yeniden başlayacak
  otaActive = false;
  linkSetHold(false);
}

// ---------------------------------------------------------------- ağ
static void netTick() {
  uint32_t now = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    wifiUp = true;
    Log.printf("[ağ] WiFi bağlandı: %s (%d dBm)\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    configTime(3 * 3600, 0, "pool.ntp.org", "time.google.com");     // Türkiye saati (UTC+3)
    NetOtaHooks oh;
    oh.start = otaStarting;
    oh.progress = otaProgress;
    oh.end = otaFinished;
    NetPanelHooks ph;
    ph.durum = panelDurum;
    ph.arac = panelArac;
    netSetPanel(ph);
    netStart(oh);                                // yalnızca ilk bağlantıda kurulur
    if (netEnabled()) {                          // kablo takılı değilken adresi görmek için
      char b[64];
      snprintf(b, sizeof(b), "%s.local  %s", HOSTNAME, WiFi.localIP().toString().c_str());
      uiText("nova", b, true);
    }
  }
  if (!up && wifiUp) {
    Log.println("[ağ] WiFi koptu");
    wifiUp = false;
    lastWifiKick = now;
  }
  if (!up && now - lastWifiKick > 20000) {
    lastWifiKick = now;
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
  linkSetNetUp(up);

  if (now - lastRssi > 1000) {
    lastRssi = now;
    uiSetLink(up, up ? WiFi.RSSI() : -100);
  }
}

// ---------------------------------------------------------------- döngü işleri
static void pumpButton() {
  static bool lastBtn = true;
  static uint32_t btnAt = 0;
  bool btn = digitalRead(PIN_BUTTON);
  if (btn != lastBtn && millis() - btnAt > 40) {
    btnAt = millis();
    lastBtn = btn;
    if (!btn) linkButton();
  }
}

static void pumpStatus() {
  uint32_t now = millis();
  bool active = linkState() != LINK_IDLE;
  if (now - lastStatus < (active ? STATUS_INTERVAL_MS : 6 * STATUS_INTERVAL_MS)) return;   // uykuda 30 sn'de bir
  lastStatus = now;
  if (!linkVerbose()) return;
  Log.printf("[durum] pil %.2fV %%%d | wifi %d dBm | mik L=%u R=%u (%s) | bellek %u, PSRAM %u\n",
                batVolts(), batPercent(), WiFi.RSSI(), micLevelLeft(), micLevelRight(), micDiagText(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  if (active) linkPrintStats();
  char eb[280];
  if (echoStatusText(eb, sizeof(eb))) Log.printf("[yankı] %s\n", eb);
  if (uiStatsText(eb, sizeof(eb))) Log.printf("[ekran] %s\n", eb);
}


static void resolveUi() {
  UiState s;
  if (otaActive) {
    s = UI_OTA;
  } else if (!wifiUp) {
    s = UI_WIFI;
  } else {
    switch (linkState()) {
      case LINK_CONNECTING: s = UI_SERVER; break;
      case LINK_LISTEN:     s = linkBackendSince() ? UI_THINK : UI_LISTEN; break;   // arka model çalışırken
      case LINK_THINK:      s = UI_THINK; break;
      case LINK_SPEAK:      s = UI_SPEAK; break;
      default:              s = UI_IDLE; break;
    }
  }
  uiSetArkaModel(linkBackendSince());
  if (s != uiGetState()) uiSetState(s);
}

static void lowBatteryShutdown() {
  Log.println("[pil] kritik seviye, uykuya geçiliyor");
  linkSleep();
  hafiza::kaydet();
  uiSetState(UI_LOWBAT);
  delay(3000);
  audioStop();
  uiScreenOff();
  WiFi.disconnect(true);
  esp_deep_sleep_start();     // şalterden kapatıp açınca uyanır
}

// ---------------------------------------------------------------- bakım döngüsü
// Yalnızca WiFi + kayıt sayfası + kablosuz yükleme çalışır: kart bir sebeple normal açılamıyorsa
// kablo takmadan düzeltilmiş yazılım yüklenebilsin. Çıkış: yeni yazılım ya da 'R' (yeniden başlat).
static void maintenanceLoop(const char *screen, const char *const *lines, int nLines) {
  uiText("nova", screen, true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t lastMsg = 0;
  bool up = false;
  for (;;) {
    if (!up && WiFi.status() == WL_CONNECTED) {
      up = true;
      Log.printf("[ağ] WiFi bağlandı: %s\n", WiFi.localIP().toString().c_str());
      netStart(NetOtaHooks());
      char b[160];
      snprintf(b, sizeof(b), "%s  %s.local  %s", screen, HOSTNAME, WiFi.localIP().toString().c_str());
      uiText("nova", b, true);
    }
    netLoop();
    int c = Serial.available() ? Serial.read() : netReadCommand();
    if (c == 'R') {
      Log.println("[komut] yeniden başlatılıyor");
      delay(300);
      ESP.restart();
    }
    if (millis() - lastMsg > 15000) {
      lastMsg = millis();
      for (int i = 0; i < nLines; i++) Log.println(lines[i]);
    }
    delay(5);
  }
}

// WiFi güç tasarrufundan çıkarken erişim noktasının biriktirdiği paketler bir anda gelir ve sürücü iç RAM'den
// tampon alır; 2.6.0'da ilk şifreli bağlantı bu sırada iki kez "bellek ayrılamadı" ile düştü. Kısa bir bekleme.
static void wifiUyandir() {
#if BOSTA_WIFI_UYKU
  WiFi.setSleep(false);
  if (wifiUyuyor) {
    wifiUyuyor = false;
    delay(400);
  }
#endif
}

static void logMemBeforeConnect() {
  wifiUyandir();                                // ağ görevinde, bağlanmadan hemen önce
  Log.printf("[bellek] bağlanmadan önce iç RAM: boş %u KB, en büyük blok %u KB\n",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  // ESP-IDF'in kendi uyarıları (printf, UART0) konsol kilidini ilk yazışta kurar ve bunun için iç RAM
  // ister. 2.3.1'deki "tiT" çökmesi: iç RAM tükenmişken ağ görevi bir uyarı yazmak istedi, kilit
  // kurulamadı, abort. Kilit burada, bellek bolken kurulur.
  fputs("\n", stdout);
  fflush(stdout);
  netBegin();                        // Log'un kopyası (kayıt sayfası için) ve önceki çalışmadan kalanlar
  delay(200);
  Log.printf("\n=== Nova V2 firmware %s (nova_live: modele doğrudan) ===\n", FW_VERSION);
  Log.printf("PSRAM: %s | çekirdek %s\n", psramFound() ? "var" : "YOK (Tools > PSRAM > OPI PSRAM seç)", ESP_ARDUINO_VERSION_STR);
  netBootReport();

  prefs.begin("nova", false);
  int vol = prefs.getInt("vol", DEFAULT_VOLUME);
  int bri = prefs.getInt("bri", DEFAULT_BRIGHTNESS);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  normalParlaklik = bri;

  uiBegin();
  uiSetBrightness(bri);
  uiSetState(UI_BOOT);

  // PSRAM yoksa bellek yetmez: WiFi ve ses tamponları kurulamaz, kayıt anlamsız hatalarla dolar.
  // Çalışmaya çalışma, sebebi söyle; kablosuz yükleme yine açık.
  if (!psramFound()) {
    static const char *const L[] = {
      "[HATA] PSRAM bulunamadı: firmware yanlış kart ayarıyla yüklenmiş. Arduino IDE > Tools:",
      "[HATA]   PSRAM: OPI PSRAM | Flash Size: 16MB (128Mb) | Partition Scheme: Custom | Flash Mode: QIO 80MHz",
      "[HATA]   Ayarları düzeltip yeniden Upload'a bas (kablosuz yükleme bu durumda da çalışır)."};
    maintenanceLoop("PSRAM kapalı: Tools > PSRAM > OPI PSRAM seçip yeniden yükle.", L, 3);
  }
  // Art arda çöküyorsa güvenli kip: yalnızca WiFi + sayfa + kablosuz yükleme (ses, uyandırma, model yok).
  if (netCrashStreak() >= SAFE_MODE_CRASHES) {
    static const char *const L[] = {
      "[güvenli kip] kart art arda çöktüğü için yalnızca WiFi, kayıt sayfası ve kablosuz yükleme açık.",
      "[güvenli kip] Çökmenin sebebi yukarıdaki [önceki] / [çökme] satırlarında. Düzeltilmiş yazılımı yükle",
      "[güvenli kip] ya da 'R' ile yeniden başlat (normal açılmayı bir kez daha dener)."};
    maintenanceLoop("Güvenli kip: art arda çöktü. Yeni yazılım yükle ya da R.", L, 3);
  }

  // Uzun ömürlü büyük tamponlar PSRAM'de: iç RAM şifreli bağlantıya ve WiFi'ye kalsın
  toneBuf = (int16_t *)hookAlloc(TONE_BYTES);
  spkSetVolume(vol);
  audioOk = audioBegin();
  if (!audioOk) Log.println("[ses] I2S başlatılamadı!");

  LinkHooks hooks;
  hooks.nowMs = hookNow;
  hooks.rnd = hookRnd;
  hooks.idle = hookIdle;
  hooks.systemText = buildSystemText;
  hooks.bigAlloc = hookAlloc;
  hooks.readyChime = readyChime;
  hooks.beforeConnect = logMemBeforeConnect;
  hooks.usage = hookUsage;
  hooks.echoCancel = micSetEchoCancel;
  hooks.echoActive = micEchoActive;
  // Aşama 5: cihaz araçları, hafıza, zamanlayıcılar
  hooks.toolsJson = ARAC_TANIMLARI;
  hooks.tool = hookTool;
  hooks.heard = hookHeard;
  hooks.line = hookLine;
  hooks.backendText = buildBackendText;
  {
    AracOrtam o;
    o.ses = aracSes;
    o.sesAyarla = aracSesAyarla;
    o.parlaklik = aracParlaklik;
    o.parlaklikAyarla = aracParlaklikAyarla;
    o.ayar = aracAyar;
    o.durum = aracDurum;
    o.simdi = simdiEpoch;
    o.ms = aracMs;
    aracBegin(o);
    if (hafiza::begin(hafiza::flasDepo(), YEREL_SAAT_FARKI))
      Log.printf("[hafıza] %d not, %d zamanlayıcı/alarm\n", hafiza::notSayisi(), hafiza::zamanlayiciSayisi());
    else
      Log.println("[hafıza] HATA: bellek ayrılamadı; hafıza ve zamanlayıcılar çalışmayacak");
    // 2.7.0: bölüm tablosu (partitions.csv). Tablo yalnızca USB yüklemesiyle değişir.
    const esp_partition_t *uyg = esp_ota_get_running_partition();
    char dd[96];
    hafiza::flasDurum(dd, sizeof dd);
    Log.printf("[depo] yazılım bölümü %u KB, %s\n", uyg ? (unsigned)(uyg->size / 1024) : 0u, dd);
    if (uyg && uyg->size < 6u * 1024u * 1024u)
      Log.println("[depo] eski bölüm tablosu (3 MB): yeni tabloya geçmek için bir kez USB ile yükle, Partition Scheme: Custom");
    if (prefs.isKey("gundem")) {                 // son gündem özeti (saatine göre yeniden çekilip çekilmeyeceği belirlenir)
      String g = prefs.getString("gundem", "");
      if (g.length()) gundemSakla(g.c_str(), prefs.getLong64("gundemT", 0));
    }
  }
  {
    UsageTally u = usageLoad();
    if (u.sessions) usagePrint(u, "son kayıtlı gün");
  }
  wakeSetMode(prefs.getInt("wake", WAKE_DEFAULT_MODE));
  if (wakeBegin()) {
    hooks.wakeArm = wakeArm;
    hooks.wakeFeed = wakeFeed;
    linkSetWakeEnabled(wakeMode() != WAKE_OFF);
    uiSetBootHint(wakeMode() == WAKE_OFF);
    Log.printf("[uyan] \"Hey Nova\": %s\n", wakeModeName(wakeMode()));
  }
  if (!linkBegin(&transport, hooks)) Log.println("[bağ] bellek ayrılamadı!");
  applyStoredVoice();
  Log.printf("[bağ] model: %s %s, ses %s\n", linkProviderName(), linkModelName(), linkVoiceName());
  // Yankı ölçümü ve araya girme (Aşama 3)
  int echoD = prefs.getInt("echoD", -1);
  if (echoD >= 0) {
    float resid[4];
    if (prefs.getBytesLength("echoRs") == sizeof(resid)) prefs.getBytes("echoRs", resid, sizeof(resid));
    else for (int i = 0; i < 4; i++) resid[i] = prefs.getFloat("echoR", -20.0f);    // 2.4.0-2.4.1 kaydı: tek değer
    // 2.4.3 öncesi ölçüm kısılmamış girişle yapılmıştı: artık/referans oranı aynı pay kadar aşağı kayar
    if (prefs.getInt("echoV", 1) < 2) for (int i = 0; i < 4; i++) resid[i] -= ECHO_IN_HEADROOM_DB;
    echoSetCalibration(echoD, resid);
  }
  echoSetNlp(ECHO_NLP_DEFAULT);                 // 2.6.1: ayar kaldırıldı, hep normal (eski kayıt okunmaz)
  linkSetBargeIn(prefs.getBool("barge", ECHO_DEFAULT_ON != 0));
  if (echoD < 0) Log.println("[yankı] yankı ölçümü henüz yapılmadı (panelde \"Yankı ölç\")");
  // Ayarların tek satırlık özeti (hepsi kartta saklanır; sayfadan ya da sesle değişir)
  Log.printf("[ayarlar] uyandırma: %s | araya girme: %s | ses: %%%d | parlaklık: %%%d | konuşma sesi: %s\n",
             wakeModeName(wakeMode()), linkBargeIn() ? "açık" : "kapalı", spkGetVolume(), normalParlaklik, linkVoiceName());
  warnIfKeyMissing();
  xTaskCreatePinnedToCore(netTask, "nova_net", 16384, nullptr, 4, nullptr, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // ses için düşük gecikme
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiKick = millis();
  Log.printf("[ağ] WiFi'ye bağlanılıyor: %s\n", WIFI_SSID);
}

void loop() {
  static bool fine = false, confirmed = false;
  if (!fine && millis() > 60000) { fine = true; netStartedFine(); }                    // çökme sayacı sıfırlanır
  if (!confirmed && wifiUp && millis() > 20000) { confirmed = true; netConfirmFirmware(); }
  netLoop();
  serialCommands();
  netTick();
  pumpButton();
  pumpBootChime();
  pumpMicHealth();
  pumpMicWatch();
  pumpZamanlayici();
  pumpOturumSonu();
  pumpBostaGuc();
  pumpKayit();
  pumpStatus();
  resolveUi();
  if (batIsLow()) lowBatteryShutdown();
  delay(2);
}
