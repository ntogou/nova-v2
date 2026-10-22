// Nova V2 — ses katmanı
//
// Mikrofonlar: I2S1, 16 kHz, 32 bit stereo. İki INMP441 aynı hatta,
//              biri sol biri sağ kanalı dolduruyor.
// Amfiler:     I2S0, 24 kHz, 16 bit stereo. Tek I2S çıkışı iki amfinin
//              pinlerine birden yönlendiriliyor. Amfilerin SD pini boşta
//              olduğu için (L+R)/2 çalıyorlar; iki kanala aynı örnek yazılır.
#include "audio.h"
#include "netlog.h"
#include <math.h>
#include "play_engine.h"
#include "resample.h"
#include "loudness.h"
#include "psbuf.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "soc/i2s_periph.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "echo_math.h"
extern "C" {
#include "esp_aec.h"
}

static i2s_chan_handle_t txChan = nullptr;
static i2s_chan_handle_t rxChan = nullptr;
static QueueHandle_t micQ = nullptr;
static volatile bool running = false;

static PlayEngine engine;
static uint8_t *ringMem = nullptr;
static size_t ringCap = 0;
static volatile int volumePct = DEFAULT_VOLUME;

static volatile uint16_t lvlMic = 0, lvlSpk = 0, lvlMicL = 0, lvlMicR = 0;
static Loudness loud;        // çalma yolunda: süzgeç + kazanç + sınırlayıcı + ses ayarı

// ---------- hoparlör arayüzü ----------
size_t spkWrite(const uint8_t *data, size_t len) { return engine.write(data, len); }
void spkFlush() { engine.flush(); }
void spkMarkEnd() { engine.markEnd(); }

void spkSetVolume(int percent) {
  percent = constrain(percent, 0, 100);
  volumePct = percent;
  // Kulağa doğrusal gelsin diye karesel eğri: %50 = 0.25 (V1'deki seviye)
  float g = percent / 100.0f;
  engine.setGainQ15(32768);             // çalma motoru sesi olduğu gibi verir; ses ayarı sınırlayıcıdan sonra uygulanır
  loud.setVolume(g * g);
}

int spkGetVolume() { return volumePct; }
uint32_t spkBufferedMs() { return (uint32_t)(engine.count() / 2) * 1000UL / SPK_RATE; }
size_t spkFree() { size_t used = engine.count(); return ringCap > used + 2 ? ringCap - 2 - used : 0; }
bool audioTakePlayStart() { return engine.takeStart(); }
bool audioTakePlayEnd() { return engine.takeEnd(); }

uint16_t micLevel()      { return lvlMic; }
uint16_t spkLevel()      { return lvlSpk; }
uint16_t micLevelLeft()  { return lvlMicL; }
uint16_t micLevelRight() { return lvlMicR; }
uint32_t spkOverflowCount() { return engine.overflowCount(); }
uint32_t spkUnderrunCount() { return engine.underrunCount(); }
void spkSetBoostDb(float db) { loud.setBoostDb(db); }
void spkSetPrebufMs(int ms) {
  if (ms < 20) ms = 20;
  engine.setPrebuf((size_t)SPK_RATE * 2 * (size_t)ms / 1000);
}

// Görevlerin çalışma tamponları PSRAM'de (audioBegin alır): iç RAM şifreli bağlantıya kalsın.
// I2S sürücüsü okurken / yazarken kendi DMA tamponuna kopyaladığı için bunların PSRAM'de olması sorun değil.
static int16_t *playMono = nullptr;           // SPK_BLOCK örnek
static int16_t *playStereo = nullptr;         // SPK_BLOCK * 2 örnek
static int32_t *micRaw = nullptr;             // MIC_RAW_N kelime: 20 ms stereo
static int16_t *micFrame = nullptr;           // MIC_FRAME_SAMPLES örnek
static uint8_t *micDrop = nullptr;            // MIC_FRAME_BYTES
static const size_t MIC_RAW_N = 320 * 2;

// ---------- yankı silme: referans ----------
// Hoparlöre giden son ses (ses ayarı ve gürlük artırmadan sonra) 16 kHz'e indirilip bu halkada tutulur.
// Hoparlör ve mikrofon aynı saatten türer (24 ve 16 kHz, tam 3:2) ve ikisi de hiç durmadan akar; bu
// yüzden iki sayaç arasındaki fark sabit kalır. Fark, iki görevin son okuma / yazma anlarından ölçülür
// (refOff). Hoparlöre yazılan bir örneğin mikrofona ulaşma süresi (DMA kuyrukları + hava) ise 'a'
// ölçümüyle bulunur (echoDelay). Mikrofon örneği m'nin yankısı, referans örneği m + refOff - echoDelay'dir.
static const uint32_t REF_N = 65536;            // ~4 sn, 2'nin kuvveti
// Yankı silmenin girişindeki pay (config.h ECHO_IN_HEADROOM_DB): giriş kısılır, çıkış aynı kadar yükseltilir.
// Seviye satırları kısılmamış mikrofonun ölçeğinde yazılır (ECHO_HEAD_DB eklenir).
static const float ECHO_IN_GAIN = 1.0f / (float)(1 << (ECHO_IN_HEADROOM_DB / 6));
static const float ECHO_HEAD_DB = 6.0206f * (ECHO_IN_HEADROOM_DB / 6);
static inline int16_t sat16(float m) { return m > 32767.0f ? 32767 : (m < -32768.0f ? -32768 : (int16_t)lroundf(m)); }
static int16_t *refRing = nullptr;
static volatile uint32_t refTotal = 0;          // halkaya yazılan toplam örnek
static Downsampler24to16 refDown;
static volatile uint32_t refSeq = 0;            // tek sayıdayken yazılıyor (okuyan yarım değer almasın)
static volatile int64_t refStampUs = 0;         // son hoparlör yazımının anı
static volatile uint32_t refStampTotal = 0;     // o andaki referans sayacı

static bool refStamp(int64_t &us, uint32_t &tot) {
  for (int i = 0; i < 4; i++) {
    uint32_t s = refSeq;
    if (s & 1) { taskYIELD(); continue; }
    us = refStampUs;
    tot = refStampTotal;
    if (refSeq == s) return true;
  }
  return false;
}

// Ses kısma (2.8.7): mikrofon görevi yazar, çalma görevi okur
static volatile uint32_t duckUntil = 0;
static volatile bool aecOn;
static const float DUCK_LIN = ECHO_DUCK_DB > 0 ? powf(10.0f, -ECHO_DUCK_DB / 20.0f) : 1.0f;

// ---------- çalma görevi ----------
static void playTask(void *) {
  int16_t *const mono = playMono;
  int16_t *const stereo = playStereo;
  const size_t stereoBytes = SPK_BLOCK * 2 * sizeof(int16_t);
  uint32_t actBlocks = 0, quietBlocks = 0;
  while (running) {
    uint16_t inPeak = engine.renderBlock(stereo, mono, millis());
    // Gürlük artırma: motorun çıkışı (iki kanalda aynı örnek) tek kanal olarak işlenir.
    for (int i = 0; i < SPK_BLOCK; i++) mono[i] = stereo[2 * i];
    loud.statsEnable(inPeak > 0);
    loud.process(mono, mono, SPK_BLOCK);
    {                                           // ses kısma: kullanıcı Nova'nın üstüne konuşmaya başlıyor olabilir
      static echomath::DuckGain dk;
      float hedef = (aecOn && (int32_t)(duckUntil - millis()) > 0) ? DUCK_LIN : 1.0f;
      float g0 = dk.g, g1 = dk.step(hedef);
      if (g0 < 0.999f || g1 < 0.999f)
        for (int i = 0; i < SPK_BLOCK; i++) {
          float g = g0 + (g1 - g0) * (i + 1) / SPK_BLOCK;
          mono[i] = (int16_t)lroundf(mono[i] * g);
        }
    }
    uint16_t peak = 0;
    for (int i = 0; i < SPK_BLOCK; i++) {
      int16_t v = mono[i];
      uint16_t a = (uint16_t)(v < 0 ? -(int32_t)v : v);
      if (a > peak) peak = a;
      stereo[2 * i] = v;
      stereo[2 * i + 1] = v;
    }
    lvlSpk = peak;
    if (refRing) {                              // yankı silmenin referansı: hoparlöre giden sesin 16 kHz kopyası
      int16_t r16[SPK_BLOCK * 2 / 3];
      size_t n16 = refDown.process(mono, SPK_BLOCK, r16);
      uint32_t t = refTotal;
      for (size_t i = 0; i < n16; i++) refRing[(t + i) & (REF_N - 1)] = r16[i];
      refTotal = t + (uint32_t)n16;
    }
    // Bir konuşma bitince (1,3 sn sessizlik) gelen sesin seviyesini yaz: kazancı ayarlamak için.
    // 1,3 sn: cümle arası duraklamada satır bölünmesin (GPT-Live yarım saniyeyi aşan duraklamalar yapıyor).
    if (inPeak > 0) { actBlocks++; quietBlocks = 0; }
    else if (actBlocks && ++quietBlocks >= 130) {
      if (actBlocks >= 50)
        Log.printf("[ses] çalınan %.1f sn | gelen ses: tepe %.1f dB, ortalama %.1f dB | sınırlayıcı en çok %.1f dB kıstı | ses ayarı %%%d\n",
                      loud.statSamples() / (float)SPK_RATE, loud.statPeakDb(), loud.statRmsDb(), loud.statLimitDb(), (int)volumePct);
      loud.clearStats();
      actBlocks = 0;
    }
    size_t written = 0;
    // Ses yokken de sessizlik yazılır: saat hiç durmaz, amfi açılıp kapanmaz.
    i2s_channel_write(txChan, stereo, stereoBytes, &written, portMAX_DELAY);
    refSeq = refSeq + 1;                        // yazım bitti: an ve sayaç birlikte kaydedilir
    refStampUs = esp_timer_get_time();
    refStampTotal = refTotal;
    refSeq = refSeq + 1;
  }
  vTaskDelete(nullptr);
}

// ---------- mikrofon öz-testi ve tanısı ----------
// Açılışta mikrofonlardan veri gelip gelmediğine bakılır. Gelmiyorsa ESP kendi
// pinlerini ölçer: saat gerçekten çıkıyor mu, hatlar bir yere kısa mı, teller
// başka sırada mı. Sonuç Serial'e "[mik]" satırları olarak yazılır.
static volatile uint8_t micState = MIC_UNKNOWN;
static volatile bool micBusy = false;
static char micText[180] = "test ediliyor";
static bool rxOn = false;
static int micPinSck = PIN_MIC_SCK, micPinWs = PIN_MIC_WS, micPinSd = PIN_MIC_SD;

static volatile bool micRetestReq = false;
// Hangi mikrofon kullanılıyor: 0 = ikisinin ortalaması, 1 = yalnız sol, 2 = yalnız sağ.
// MIC_MODE 0 iken canlı izleme belirler: biri bozulursa sağlam olanla devam edilir.
static volatile int micUse = MIC_MODE;
uint8_t micDiag() { return micState; }
// Canlı izleme: son 250 ms'lik pencerede kanal gerçek mikrofon verisi taşıyor mu
static volatile bool liveAlive[2] = {false, false};
static volatile uint8_t liveChgPct[2] = {0, 0};
bool micAlive(int ch) { return liveAlive[ch & 1]; }
int micChangePct(int ch) { return liveChgPct[ch & 1]; }
void micRetest() { micRetestReq = true; }
int micInUse() { return micUse; }
const char *micDiagText() { return micText; }

static const int MIC_PINS[3] = {PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD};
static const char *const MIC_PIN_NAMES[3] = {"SCK", "WS", "SD"};

static void rxEnable()  { if (!rxOn) { i2s_channel_enable(rxChan);  rxOn = true;  } }
static void rxDisable() { if (rxOn)  { i2s_channel_disable(rxChan); rxOn = false; } }

// Üç mikrofon pinini de boşa alır (giriş, çekme yok, normal sürüş gücü).
static void micPinsRelease() {
  for (int i = 0; i < 3; i++) {
    gpio_num_t p = (gpio_num_t)MIC_PINS[i];
    gpio_set_pull_mode(p, GPIO_FLOATING);
    gpio_set_drive_capability(p, GPIO_DRIVE_CAP_DEFAULT);
    gpio_set_direction(p, GPIO_MODE_INPUT);
  }
}

// Saat hatlarının (SCK, WS) sürüş gücü: 0 en zayıf (~5 mA) ... 3 en güçlü (~40 mA).
// Hatta bir yük varsa güçlü sürüş, mikrofona ulaşan saat seviyesini biraz yükseltir.
static volatile int micDrive = MIC_DRIVE;
static volatile bool micAutoOff = false;     // tarama sırasında kendiliğinden test koşmasın
static void applyMicDrive(int sck, int ws) {
  gpio_set_drive_capability((gpio_num_t)sck, (gpio_drive_cap_t)micDrive);
  gpio_set_drive_capability((gpio_num_t)ws, (gpio_drive_cap_t)micDrive);
}

// Mikrofon I2S'inin örnekleme hızı. Normalde MIC_RATE (16 kHz, SCK 1,024 MHz). Mikrofonlar
// tam hızda cevap vermezse yarı hıza (8 kHz, SCK 512 kHz) inilir ve ses yazılımda 16 kHz'e tamamlanır.
static volatile int micRate = MIC_RATE;
static volatile bool micCfgReq = false;      // başka görevden istenen ayar (karşılaştırma testi için)
static volatile int micCfgHz = MIC_RATE, micCfgDrive = MIC_DRIVE;
static void rxEnable();
static void rxDisable();
static void micSetClock(int hz) {
  rxDisable();
  i2s_std_clk_config_t c = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)hz);
  i2s_channel_reconfig_std_clock(rxChan, &c);
  rxEnable();
  micRate = hz;
}
void micSetConfig(int hz, int drive) {
  micCfgHz = hz;
  micCfgDrive = drive < 0 ? 0 : (drive > 3 ? 3 : drive);
  micCfgReq = true;
}
int micClockHz() { return micRate; }

// Hangi mikrofonlardan veri geldiğini anlatan metinler (bit 0 = sol, bit 1 = sağ).
static const char *const MIC_MASK_TXT[4] = {
  "mikrofonlardan veri gelmiyor (çalışırken kesildi)",
  "sol tamam, SAĞ MİKROFONDAN VERİ YOK", "sağ tamam, SOL MİKROFONDAN VERİ YOK", "tamam"};
// Normal ayarda değilsek durum metninin sonuna eklenen not.
static const char *rateNote() {
  static char b[64];
  int hz = micRate, dr = micDrive;
  const char *d = dr == MIC_DRIVE ? "" : (dr < MIC_DRIVE ? ", zayıf sürüş" : ", güçlü sürüş");
  if (hz == MIC_RATE && dr == MIC_DRIVE) return "";
  if (hz == MIC_RATE) snprintf(b, sizeof(b), " (%s)", d + 2);
  else if (hz == MIC_RATE / 2) snprintf(b, sizeof(b), " (yarı hızda, 8 kHz%s)", d);
  else snprintf(b, sizeof(b), " (%d Hz'de%s)", hz, d);
  return b;
}

static void micSetPins(int sck, int ws, int sd) {
  rxDisable();
  micPinsRelease();                 // eski saat çıkışı başka pinde asılı kalmasın
  i2s_std_gpio_config_t g = {};
  g.mclk = I2S_GPIO_UNUSED;
  g.bclk = (gpio_num_t)sck;
  g.ws = (gpio_num_t)ws;
  g.dout = I2S_GPIO_UNUSED;
  g.din = (gpio_num_t)sd;
  i2s_channel_reconfig_std_gpio(rxChan, &g);
  rxEnable();
  micPinSck = sck; micPinWs = ws; micPinSd = sd;
  applyMicDrive(sck, ws);
}

int micGetDrive() { return micDrive; }
// Açılıştan beri sayaçlar: 250 ms'lik pencere sayısı, sol / sağ mikrofonun canlı olduğu pencereler, kesinti sayısı.
static volatile uint32_t healthWin = 0, healthL = 0, healthR = 0, healthOutages = 0;
void micHealth(uint32_t *windows, uint32_t *left, uint32_t *right, uint32_t *outages) {
  *windows = healthWin; *left = healthL; *right = healthR; *outages = healthOutages;
}
void micAutoTest(bool on) { micAutoOff = !on; }

struct MicStats {
  uint32_t n;
  int32_t mn[2], mx[2];
  uint32_t changes[2];              // bir önceki örnekten farklı olanlar
  uint32_t big[2];                  // tam ölçeğe yakın olanlar
  uint32_t raw[2][3];               // ilk üç ham kelime
};

// ms boyunca okur ve kanal başına istatistik çıkarır.
static void micProbe(int ms, MicStats &s) {
  static int32_t *probeBuf = nullptr;           // PSRAM'de, ilk testte alınır
  int32_t *buf = psLazy(probeBuf, MIC_RAW_N);
  memset(&s, 0, sizeof(s));
  if (!buf) return;
  s.mn[0] = s.mn[1] = INT32_MAX;
  s.mx[0] = s.mx[1] = INT32_MIN;
  int32_t prev[2] = {0, 0};
  int skip = 6;                     // tamponda bekleyen eski 120 ms atılır
  uint32_t start = millis();
  while (millis() - start < (uint32_t)ms + 120) {
    size_t got = 0;
    if (i2s_channel_read(rxChan, buf, MIC_RAW_N * sizeof(int32_t), &got, pdMS_TO_TICKS(200)) != ESP_OK) continue;
    if (skip > 0) { skip--; continue; }
    for (size_t i = 0; i < got / 8; i++) {
      for (int c = 0; c < 2; c++) {
        if (s.n < 3) s.raw[c][s.n] = (uint32_t)buf[2 * i + c];
        int32_t v = buf[2 * i + c] >> 8;          // 24 bit örnek
        if (v < s.mn[c]) s.mn[c] = v;
        if (v > s.mx[c]) s.mx[c] = v;
        if (v != prev[c]) s.changes[c]++;
        if (v >= (1 << 22) || v <= -(1 << 22)) s.big[c]++;
        prev[c] = v;
      }
      s.n++;
    }
  }
}

// Canlı mikrofon: neredeyse her örnek bir öncekinden farklıdır (kendi gürültüsü)
// ve değerler tam ölçekte gezmez. Boştaki ya da kısa hat bunu sağlamaz.
static bool chanAlive(const MicStats &s, int c) {
  if (s.n < 1000) return false;
  if ((int64_t)s.mx[c] - s.mn[c] <= 32) return false;      // 0 / -1 arasında gidip gelen boş hat
  return s.changes[c] * 100 / s.n >= 60 && s.big[c] * 100 / s.n < 10;
}

static void printStats(const char *baslik, const MicStats &s) {
  if (s.n == 0) { Log.printf("[mik] %s: I2S'ten hiç örnek okunamadı\n", baslik); return; }
  Log.printf("[mik] %s: %u örnek | L %ld..%ld değişim %%%u ham %08lx %08lx %08lx | R %ld..%ld değişim %%%u ham %08lx %08lx %08lx\n",
                baslik, (unsigned)s.n,
                (long)s.mn[0], (long)s.mx[0], (unsigned)(s.n ? s.changes[0] * 100 / s.n : 0),
                (unsigned long)s.raw[0][0], (unsigned long)s.raw[0][1], (unsigned long)s.raw[0][2],
                (long)s.mn[1], (long)s.mx[1], (unsigned)(s.n ? s.changes[1] * 100 / s.n : 0),
                (unsigned long)s.raw[1][0], (unsigned long)s.raw[1][1], (unsigned long)s.raw[1][2]);
}

// Pinin 2000 okumada yüzde kaç "1" olduğu. Okuma aralığı bilerek düzensiz:
// saat sinyaliyle aynı adıma düşüp hep aynı yeri okumasın.
static int pinHighPct(int pin, int samples = 2000) {
  int hi = 0;
  for (int i = 0; i < samples; i++) {
    hi += gpio_get_level((gpio_num_t)pin);
    esp_rom_delay_us(17 + (i & 15));
  }
  return hi * 100 / samples;
}

// WS pinindeki yükselen kenarları 20 ms sayar. 16000 civarı çıkmalı.
static int wsFreqHz(int pin) {
  int edges = 0, last = gpio_get_level((gpio_num_t)pin);
  int64_t t0 = esp_timer_get_time();
  while (esp_timer_get_time() - t0 < 20000) {
    int v = gpio_get_level((gpio_num_t)pin);
    if (v && !last) edges++;
    last = v;
  }
  return edges * 50;
}

// Bir hattı en zayıf sürüşle (yaklaşık 5 mA) önce 1'e, sonra 0'a iter ve
// bıraktıktan sonra ne olduğuna bakar. Sağlam, boştaki bir hatta sonuç
// it1=100 tut1=100 it0=0 tut0=0 olur. Seri 100 ohm dirençler ve 5 mA sınırı
// yüzünden hat kısa olsa bile pine ya da mikrofona zarar vermez.
struct LineRes { int it1, tut1, it0, tut0; };

static void lineTest(int pin, LineRes &r) {
  gpio_num_t p = (gpio_num_t)pin;
  gpio_set_drive_capability(p, GPIO_DRIVE_CAP_0);
  gpio_set_pull_mode(p, GPIO_PULLUP_ONLY);
  gpio_set_level(p, 1);
  gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT);
  esp_rom_delay_us(500);
  r.it1 = pinHighPct(pin, 200);                   // 1'e iterken
  gpio_set_direction(p, GPIO_MODE_INPUT);         // bırak, yalnızca iç çekme direnci kalsın
  vTaskDelay(pdMS_TO_TICKS(10));
  r.tut1 = pinHighPct(pin, 200);
  gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
  gpio_set_level(p, 0);
  gpio_set_direction(p, GPIO_MODE_INPUT_OUTPUT);
  esp_rom_delay_us(500);
  r.it0 = pinHighPct(pin, 200);                   // 0'a iterken
  gpio_set_direction(p, GPIO_MODE_INPUT);
  vTaskDelay(pdMS_TO_TICKS(10));
  r.tut0 = pinHighPct(pin, 200);
  gpio_set_pull_mode(p, GPIO_FLOATING);
  gpio_set_drive_capability(p, GPIO_DRIVE_CAP_DEFAULT);
}

// 0: temiz, 1: GND'ye sıkı kısa, 2: GND'ye dirençli çekiliyor,
// 3: 3V3'e sıkı kısa, 4: 3V3'e dirençli çekiliyor
static int lineVerdict(const LineRes &r) {
  if (r.it1 < 50) return 1;
  if (r.it0 > 50) return 3;
  if (r.tut1 < 50) return 2;
  if (r.tut0 > 50) return 4;
  return 0;
}

static const char *const LINE_VERDICT_TEXT[5] = {
  "temiz", "GND'ye KISA", "GND'ye dirençle çekiliyor", "3V3'e KISA", "3V3'e dirençle çekiliyor"
};

static void setMicResult(uint8_t state, const char *text) {
  strlcpy(micText, text, sizeof(micText));
  micState = state;
  Log.printf("[mik] SONUÇ: %s\n", micText);
}

static void pinDrive(int pin, int level) {
  gpio_num_t p = (gpio_num_t)pin;
  gpio_set_pull_mode(p, GPIO_FLOATING);
  gpio_set_drive_capability(p, GPIO_DRIVE_CAP_0);       // en zayıf sürüş (~5 mA): hiçbir şeye zarar vermez
  gpio_set_level(p, level);
  gpio_set_direction(p, GPIO_MODE_OUTPUT);
}

// Kapasite testi: hat 1'e sürülür, sonra yalnızca iç aşağı çekme dirence (~45k) bırakılır ve
// 0'a inmesi kaç mikrosaniye sürüyor ölçülür. Süre hattaki kapasiteyle orantılıdır. Karşılaştırma
// için hiçbir şeye bağlı olmayan bir pin de ölçülür. Bilgi amaçlıdır, hüküm vermez.
static int32_t fallTimeUs(int pin) {
  gpio_num_t p = (gpio_num_t)pin;
  pinDrive(pin, 1);
  vTaskDelay(pdMS_TO_TICKS(5));
  gpio_set_pull_mode(p, GPIO_PULLDOWN_ONLY);
  int64_t t0 = esp_timer_get_time();
  gpio_set_direction(p, GPIO_MODE_INPUT);
  int32_t us = -1;
  int same = 0;
  while (esp_timer_get_time() - t0 < 30000) {
    if (!gpio_get_level(p)) {
      if (++same >= 3) { us = (int32_t)(esp_timer_get_time() - t0); break; }
    } else {
      same = 0;
    }
  }
  pinDrive(pin, 0);
  return us;
}

static void micCapTest() {
  for (int i = 0; i < 3; i++) pinDrive(MIC_PINS[i], 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  // Her pin 5 kez ölçülür, en küçüğü alınır: araya giren kesmeler ölçümü yalnızca uzatabilir.
  const int pins[4] = {MIC_PINS[0], MIC_PINS[1], MIC_PINS[2], PIN_FREE_REF};
  int32_t val[4];
  for (int i = 0; i < 4; i++) {
    val[i] = -1;
    for (int k = 0; k < 5; k++) {
      int32_t us = fallTimeUs(pins[i]);
      if (us >= 0 && (val[i] < 0 || us < val[i])) val[i] = us;
    }
  }
  gpio_set_pull_mode((gpio_num_t)PIN_FREE_REF, GPIO_FLOATING);
  gpio_set_direction((gpio_num_t)PIN_FREE_REF, GPIO_MODE_INPUT);
  char txt[4][20];
  for (int i = 0; i < 4; i++) {
    if (val[i] < 0) snprintf(txt[i], sizeof(txt[i]), "30 ms'de inmedi");
    else snprintf(txt[i], sizeof(txt[i]), "%ld us", (long)val[i]);
  }
  Log.printf("[mik] kapasite testi (1'den bırakılıp zayıfça 0'a çekildi, iniş süresi, 5 ölçümün en kısası): SCK %s | WS %s | SD %s | boş pin GPIO%d %s\n",
                txt[0], txt[1], txt[2], PIN_FREE_REF, txt[3]);
}

static int popcount32(uint32_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

// İç döngü testi: mikrofon olmadan ESP'nin alıcısını ve yazılımı sınar. ESP'nin kendi
// ürettiği WS sinyali, kartın içinde (pinlerde tel oynamadan) I2S veri girişine verilir.
// WS sol kanal boyunca 0, sağ kanal boyunca 1'dir; bu yüzden sol kelimeler 0'lardan,
// sağ kelimeler 1'lerden oluşmalı (kenardaki tek bit farklı olabilir). Böyle okunuyorsa
// saat üretimi, veri girişi, I2S çevre birimi ve okuma kodu doğru çalışıyor demektir.
static bool micLoopback() {
  uint32_t dinSig = i2s_periph_signal[I2S_NUM_1].data_in_sig;
  gpio_input_enable((gpio_num_t)PIN_MIC_WS);
  esp_rom_gpio_connect_in_signal(PIN_MIC_WS, dinSig, false);
  vTaskDelay(pdMS_TO_TICKS(60));
  MicStats lb;
  micProbe(200, lb);
  esp_rom_gpio_connect_in_signal(PIN_MIC_SD, dinSig, false);       // veri girişini mikrofon hattına geri ver
  int onesL = popcount32(lb.raw[0][2]), onesR = popcount32(lb.raw[1][2]);
  bool steady = lb.n > 1000 && lb.changes[0] <= 2 && lb.changes[1] <= 2;
  bool pattern = (onesL <= 2 && onesR >= 30) || (onesR <= 2 && onesL >= 30);
  bool ok = steady && pattern;
  Log.printf("[mik] iç döngü testi (ESP kendi WS sinyalini veri girişine verdi): %u örnek, sol kelime %08lx, sağ kelime %08lx, değişen %u/%u\n",
                (unsigned)lb.n, (unsigned long)lb.raw[0][2], (unsigned long)lb.raw[1][2],
                (unsigned)lb.changes[0], (unsigned)lb.changes[1]);
  if (ok) Log.println("[mik]   -> ALICI SAĞLAM: ESP ve yazılım veri girişindeki bitleri doğru okuyor (beklenen: biri 0'lar, diğeri 1'ler).");
  else    Log.println("[mik]   -> ALICI SORUNLU: beklenen desen okunmadı. Hata ESP ya da yazılım tarafında; bu satırları gönder.");
  return ok;
}

static void micSelfTest() {
  MicStats st;
  char msg[180];
  if (micRate != MIC_RATE) micSetClock(MIC_RATE);  // test hep tam hızda ve varsayılan sürüş gücünde yapılır
  micDrive = MIC_DRIVE;
  applyMicDrive(PIN_MIC_SCK, PIN_MIC_WS);
  vTaskDelay(pdMS_TO_TICKS(450));                 // mikrofonların uyanma süresi
  micProbe(500, st);
  bool l = chanAlive(st, 0), r = chanAlive(st, 1);
  if (l || r) {
    printStats("veri", st);
    setMicResult(MIC_OK, MIC_MASK_TXT[(l ? 1 : 0) | (r ? 2 : 0)]);
    return;
  }

  Log.println("[mik] ---- mikrofonlardan veri yok, ESP pinlerini kendisi ölçüyor (4 sn kadar) ----");
  printStats("ham veri", st);
  bool garbage = st.n && (st.changes[0] * 100 / st.n >= 5 || st.changes[1] * 100 / st.n >= 5);

  // 1) ESP saat üretiyor mu? Çıkış pinlerinin girişini de açıp kendi sinyalimizi okuyoruz.
  gpio_input_enable((gpio_num_t)PIN_MIC_SCK);
  gpio_input_enable((gpio_num_t)PIN_MIC_WS);
  int sckPct = pinHighPct(PIN_MIC_SCK);
  int wsPct = pinHighPct(PIN_MIC_WS);
  int wsHz = wsFreqHz(PIN_MIC_WS);
  bool clkOk = sckPct >= 20 && sckPct <= 80 && wsPct >= 20 && wsPct <= 80 && wsHz > 14000 && wsHz < 18000;
  Log.printf("[mik] ESP saat çıkışı: SCK(GPIO%d) %%%d, WS(GPIO%d) %%%d, WS frekansı %d Hz -> %s\n",
                PIN_MIC_SCK, sckPct, PIN_MIC_WS, wsPct, wsHz,
                clkOk ? "SAAT VAR (beklenen: %50, %50, 16000)" : "SAAT YOK");

  // 2) Saatler çalışırken veri hattı. ESP hattı zayıfça 1'e iter: mikrofon hattı 0'da tutuyorsa
  //    (çip ayakta ama susuyor) ESP kazanamaz (it1 düşük); hat boştaysa ESP kazanır (it1 = 100).
  int sdBos = pinHighPct(PIN_MIC_SD);
  LineRes sdRun;
  lineTest(PIN_MIC_SD, sdRun);
  Log.printf("[mik] SD(GPIO%d) saat varken: boşta %%%d | it1=%d tut1=%d it0=%d tut0=%d\n",
                PIN_MIC_SD, sdBos, sdRun.it1, sdRun.tut1, sdRun.it0, sdRun.tut0);

  // 2b) Mikrofonsuz iç döngü: alıcı ve yazılım sağlam mı?
  bool loopOk = micLoopback();

  // 3) Saatler durmuşken üç hattın her biri. SCK ve WS mikrofonun girişidir: saat yokken
  //    sıkıca 0'da ya da 1'de duruyorlarsa bir yere kısadır. SD mikrofonun çıkışıdır: saat
  //    durduğunda çalışan bir mikrofon hattı son bitte tutabilir, bu kısa sayılmaz.
  rxDisable();
  micPinsRelease();
  vTaskDelay(pdMS_TO_TICKS(120));
  LineRes lr[3];
  int verdict[3];
  for (int i = 0; i < 3; i++) {
    lineTest(MIC_PINS[i], lr[i]);
    verdict[i] = lineVerdict(lr[i]);
    const char *vt = LINE_VERDICT_TEXT[verdict[i]];
    if (i == 2 && (verdict[i] == 1 || verdict[i] == 3)) vt = "mikrofon hattı tutuyor (kısa değil, bilgi)";
    Log.printf("[mik] hat %s(GPIO%d) saat yokken: it1=%d tut1=%d it0=%d tut0=%d -> %s\n",
                  MIC_PIN_NAMES[i], MIC_PINS[i], lr[i].it1, lr[i].tut1, lr[i].it0, lr[i].tut0, vt);
  }

  // 3b) Kapasite testi (bilgi)
  micCapTest();
  micPinsRelease();

#if MIC_TRY_PIN_ORDERS
  // 4) Teller başka sırada mı lehimlenmiş? Üç pinin diğer beş sıralaması denenir.
  //    Saat pinleri bu sırada en zayıf sürüşte: yanlış pine saat gitse de zararsız.
  static const uint8_t PERM[5][3] = {{1, 0, 2}, {0, 2, 1}, {2, 1, 0}, {1, 2, 0}, {2, 0, 1}};
  bool found = false;
  for (int k = 0; k < 5 && running; k++) {
    int sck = MIC_PINS[PERM[k][0]], ws = MIC_PINS[PERM[k][1]], sd = MIC_PINS[PERM[k][2]];
    micSetPins(sck, ws, sd);
    gpio_set_drive_capability((gpio_num_t)sck, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability((gpio_num_t)ws, GPIO_DRIVE_CAP_0);
    gpio_set_pull_mode((gpio_num_t)sd, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(450));
    MicStats ps;
    micProbe(350, ps);
    bool ok = chanAlive(ps, 0) || chanAlive(ps, 1);
    Log.printf("[mik] tel sırası denemesi SCK=GPIO%d WS=GPIO%d SD=GPIO%d: %s\n", sck, ws, sd, ok ? "VERİ GELDİ" : "yok");
    if (ok) {
      found = true;
      gpio_set_drive_capability((gpio_num_t)sck, GPIO_DRIVE_CAP_DEFAULT);
      gpio_set_drive_capability((gpio_num_t)ws, GPIO_DRIVE_CAP_DEFAULT);
      gpio_set_pull_mode((gpio_num_t)sd, GPIO_FLOATING);
      snprintf(msg, sizeof(msg), "teller farklı sırada, bu açılış için düzeltildi. config.h: PIN_MIC_SCK %d, PIN_MIC_WS %d, PIN_MIC_SD %d",
               sck, ws, sd);
      setMicResult(MIC_REMAPPED, msg);
      break;
    }
  }
  if (found) return;
#endif
  micSetPins(PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);

  // 5) Hüküm. Önce kesin olanlar: saat hatlarında sıkı kısa, sonra saat, sonra geri kalan.
  for (int i = 0; i < 2; i++) {
    if (verdict[i] != 1 && verdict[i] != 3) continue;
    snprintf(msg, sizeof(msg), "HAT SORUNU: %s hattı (GPIO%d) %s", MIC_PIN_NAMES[i], MIC_PINS[i], LINE_VERDICT_TEXT[verdict[i]]);
    setMicResult(MIC_LINE_FAULT, msg);
    Log.println("[mik] Bu ölçüm ESP pininden yapıldı: kısa, pin ile mikrofon arasındaki yolda (direnç, tel, mikrofon bacağı).");
    return;
  }
  if (!clkOk) {
    snprintf(msg, sizeof(msg), "YAZILIM: ESP saat üretmiyor (SCK %%%d, WS %%%d, %d Hz)", sckPct, wsPct, wsHz);
    setMicResult(MIC_NO_CLOCK, msg);
    return;
  }
  if (!loopOk) {
    setMicResult(MIC_NO_CLOCK, "YAZILIM/ESP: iç döngü testi kaldı, alıcı kendi sinyalini doğru okuyamıyor");
    return;
  }
  bool sdActive = sdRun.it0 >= 10 || sdRun.tut0 >= 10 || (sdRun.tut1 >= 10 && sdRun.tut1 <= 90);
  if (garbage && sdBos >= 3 && sdBos <= 97) {
    setMicResult(MIC_GARBAGE, "VERİ BOZUK: mikrofon veri hattını sürüyor ama gelen bitler ses değil");
  } else if (sdActive) {
    setMicResult(MIC_SILENT, "MİKROFONLAR GİDİP GELİYOR: test başında veri yoktu, test sırasında veri hattı canlandı");
  } else if (sdRun.it1 < 80) {
    setMicResult(MIC_SILENT, "MİKROFON SUSUYOR: çip ayakta (veri hattını 0'da tutuyor) ama ses vermiyor");
  } else {
    setMicResult(MIC_SILENT, "MİKROFON SESSİZ: saat var, alıcı sağlam, mikrofonlar veri hattını sürmüyor");
  }
  Log.println("[mik] Mikrofonlar kendiliğinden geri gelirse haber verilir. Ayar karşılaştırması için 'd' (3 dakika, elsiz).");
}

// ---------- mikrofon görevi ----------
// Yüksek geçiren süzgeç (2. derece Butterworth). Mikrofonun DC kaymasını,
// 20 Hz altındaki dalgalanmayı ve 50 Hz vınlamasını atar; konuşmaya dokunmaz.
struct HighPass {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, s1 = 0, s2 = 0;
  void design(float fc, float fs) {
    float w0 = 2.0f * (float)M_PI * fc / fs;
    float c = cosf(w0), alpha = sinf(w0) * 0.70710678f;    // Q = 1/sqrt(2)
    float a0 = 1.0f + alpha;
    b0 = (1.0f + c) * 0.5f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
    s1 = s2 = 0;
  }
  inline float run(float x) {
    float y = b0 * x + s1;
    s1 = b1 * x - a1 * y + s2;
    s2 = b2 * x - a2 * y;
    return y;
  }
};

// Saat hatlarını kısa süre 0'da tutup I2S'i yeniden başlatır. Mikrofon çipi saat kesilince
// uykuya geçer, saat geri gelince baştan başlar.
static void micRestartClock(int stopMs) {
  rxDisable();
  pinDrive(PIN_MIC_SCK, 0);
  pinDrive(PIN_MIC_WS, 0);
  vTaskDelay(pdMS_TO_TICKS(stopMs));
  micSetPins(PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);   // pinleri I2S'e geri bağlar, sürüş gücünü uygular, alıcıyı açar
}

// Açılış testinin hükmü tek seferliktir; mikrofonlar sonradan gidip gelebilir. Bu yüzden
// çalışırken de her 250 ms'de bir bakılır: kanal canlı mı? Durum 1 sn (gelme) ya da 2 sn
// (kesilme) boyunca aynı kalırsa hüküm güncellenir.
//
// Sessizlik türü yoklaması: bir kanal canlı değilken saniyede bir pencere, veri hattı zayıfça
// yukarı çekilerek okunur. Mikrofon çipi ayaktaysa ama susuyorsa hattı kendi diliminde 0'da
// tutar (kelime 0 okunur); çip hattı bırakmışsa (besleme ya da saat görmüyor) çekme direnci
// kazanır ve kelime 1'lerle dolar. Sol dilimi sol mikrofon, sağ dilimi sağ mikrofon sürer.
struct LiveMon {
  uint32_t n = 0, chg[2] = {0, 0}, big[2] = {0, 0};
  int32_t prev[2] = {0, 0}, mn[2] = {INT32_MAX, INT32_MAX}, mx[2] = {INT32_MIN, INT32_MIN};
  int cand = -1, candCount = 0;
  bool probe = false;                           // bu pencere yukarı çekmeyle okunuyor
  uint8_t sinceProbe = 0;
  uint32_t pn = 0, pz[2] = {0, 0}, pf[2] = {0, 0};
  char pendCls[2] = {0, 0}, lastCls[2] = {0, 0};
  void clearWindow() {
    n = 0;
    chg[0] = chg[1] = big[0] = big[1] = 0;
    mn[0] = mn[1] = INT32_MAX;
    mx[0] = mx[1] = INT32_MIN;
    pn = 0;
    pz[0] = pz[1] = pf[0] = pf[1] = 0;
  }
  void restart() { clearWindow(); cand = -1; candCount = 0; probe = false; sinceProbe = 0; pendCls[0] = pendCls[1] = 0; }
};

static void liveReset(LiveMon &m) {
  gpio_set_pull_mode((gpio_num_t)micPinSd, GPIO_FLOATING);
  m.restart();
}

// Kesinti takibi: mikrofonlar çalışırken susarsa süresi ölçülür. (2.2.5'te kesilince saat yeniden
// başlatılıyordu; kartta işe yaramadığı görüldü, kaldırıldı.) Yalnızca mikrofon görevinden kullanılır.
static bool outage = false;
static uint32_t outageAt = 0;                   // verinin kesildiği an (yaklaşık)

static void liveApply(int mask) {
  bool wasOk = micState == MIC_OK || micState == MIC_REMAPPED;
  bool quiet = micAutoOff;                      // karşılaştırma testi sürerken günlüğü doldurma
#if MIC_MODE == 0
  int use = mask == 1 ? 1 : (mask == 2 ? 2 : 0);
  micUse = use;                                 // biri susarsa ses yalnızca sağlam olandan alınır
#endif
  if (mask == 0) {
    if (!wasOk) return;                         // zaten arızalı: açılış testinin ayrıntılı hükmü kalsın
    strlcpy(micText, MIC_MASK_TXT[0], sizeof(micText));
    micState = MIC_SILENT;
    healthOutages = healthOutages + 1;
    if (quiet) return;
    outage = true;
    outageAt = millis() - 2000;                 // hüküm 2 sn sessizlikten sonra verilir
    Log.printf("[mik] UYARI: %s.\n", micText);
    return;
  }
  if (wasOk && micState == MIC_REMAPPED && mask == 3) return;   // tel sırası notu ekranda kalsın
  char want[120];
  snprintf(want, sizeof(want), "%s%s", MIC_MASK_TXT[mask], rateNote());
  if (wasOk && !strcmp(micText, want)) return;
  strlcpy(micText, want, sizeof(micText));
  micState = MIC_OK;
  if (quiet) return;
  if (!wasOk && outage) {
    uint32_t dur = millis() - outageAt - 1000;  // geri gelme hükmü 1 sn sonra verilir
    outage = false;
    Log.printf("[mik] mikrofon verisi GELDİ: %s (kesinti %u,%u sn sürdü)\n", micText, (unsigned)(dur / 1000), (unsigned)(dur % 1000 / 100));
    return;
  }
  Log.printf("[mik] mikrofon verisi %s: %s\n", wasOk ? "değişti" : "GELDİ", micText);
}

static const char *probeText(char cls) {
  switch (cls) {
    case 'A': return "çalışıyor";
    case 'Z': return "hattı 0'da tutuyor (çip ayakta ama susuyor)";
    case 'F': return "hattı bırakmış (çip hattı sürmüyor)";
    default:  return "karışık";
  }
}

static void liveWindow(LiveMon &m) {
  int mask = 0;
  bool alive[2];
  for (int c = 0; c < 2; c++) {
    uint32_t pct = m.chg[c] * 100 / m.n;
    alive[c] = (int64_t)m.mx[c] - m.mn[c] > 32 && pct >= 60 && m.big[c] * 100 / m.n < 10;
    liveChgPct[c] = (uint8_t)pct;
    liveAlive[c] = alive[c];
    if (alive[c]) mask |= 1 << c;
  }
  healthWin = healthWin + 1;
  if (alive[0]) healthL = healthL + 1;
  if (alive[1]) healthR = healthR + 1;
  if (m.probe) {                                // bu pencere zayıf yukarı çekmeyle okundu
    gpio_set_pull_mode((gpio_num_t)micPinSd, GPIO_FLOATING);
    m.probe = false;
    m.sinceProbe = 0;
    if (m.pn > 0 && mask != 3) {
      char cls[2];
      for (int c = 0; c < 2; c++)
        cls[c] = alive[c] ? 'A' : (m.pz[c] * 100 / m.pn >= 90 ? 'Z' : (m.pf[c] * 100 / m.pn >= 90 ? 'F' : 'M'));
      bool twice = cls[0] == m.pendCls[0] && cls[1] == m.pendCls[1];       // iki yoklama üst üste aynı
      m.pendCls[0] = cls[0];
      m.pendCls[1] = cls[1];
      if (twice && (cls[0] != m.lastCls[0] || cls[1] != m.lastCls[1])) {
        m.lastCls[0] = cls[0];
        m.lastCls[1] = cls[1];
        Log.printf("[mik] sessizken veri hattına bakıldı: SOL mikrofon %s | SAĞ mikrofon %s\n", probeText(cls[0]), probeText(cls[1]));
      }
    }
  } else if (mask == 3) {
    m.sinceProbe = 0;
    m.pendCls[0] = m.pendCls[1] = m.lastCls[0] = m.lastCls[1] = 0;
#if MIC_PROBE_SILENCE
  } else if (!micAutoOff && ++m.sinceProbe >= 3) {
    m.probe = true;
    gpio_set_pull_mode((gpio_num_t)micPinSd, GPIO_PULLUP_ONLY);
#endif
  }
  m.clearWindow();
  if (mask == m.cand) m.candCount++;
  else { m.cand = mask; m.candCount = 1; }
  if (m.candCount == (mask ? 4 : 8)) liveApply(mask);
}

// ---------- yankı silme: mikrofon tarafı ----------
static volatile uint32_t micTotal = 0;          // mikrofon yolundan çıkan toplam örnek (16 kHz)
static int32_t offHist[15];
static int offN = 0, offPos = 0;
static volatile int32_t refOff = 0;             // referans sayacı - mikrofon sayacı (aynı anda)
static volatile bool refOffOk = false;
static volatile int echoDelayS = -1;            // ölçülen gecikme (örnek); < 0: ölçülmedi
static volatile float echoResid[4] = {-20.0f, -20.0f, -20.0f, -20.0f};   // ölçülen artık / referans (dB), NLP ayarına göre
static volatile int echoNlpSel = ECHO_NLP_DEFAULT;     // 0 normal, 1 güçlü, 3 kapalı (yalnızca doğrusal)
static bool aecLinear = false;                  // bu kurulumda NLP yok: yalnızca doğrusal silme
static volatile bool aecWant = false;
static aec_handle_t *aecH = nullptr;
static int aecC = 0, aecPos = 0;
static uint32_t aecStartIdx = 0;
static int16_t *aecIn = nullptr, *aecRef = nullptr, *aecOut = nullptr;
static echomath::EchoGate gate;
// Geriye dönük gönderme (2.8.7): kapı kapalıyken sıfırlanan son parçalar burada tutulur; kapı açılınca
// sözün başı ("Şe-", "Da-") kaybolmasın diye önce bunlar gönderilir.
#define LB_N ((ECHO_LOOKBACK_MS * 16 + 511) / 512)
static int16_t *lbBuf = nullptr;                // LB_N x aecC (PSRAM)
static int lbHead = 0, lbClosed = 0, lbEmit = 0;
static bool lbWasOpen = true;
static volatile uint32_t esDucks = 0, esDuckWaste = 0, esLookback = 0;
static bool aecFirst = true;
// durum satırı için (her okumada sıfırlanır)
static volatile uint32_t esChunks = 0, esFar = 0, esOpenFar = 0, esMiss = 0, esProcUs = 0, esProcMax = 0;
static volatile float esResidSum = 0;
static volatile uint32_t esResidN = 0;
static volatile float esInDb = 0, esOutDb = 0;  // Nova konuşurken: mikrofon ve yankı silme çıkışı (dBFS toplamı)
static volatile uint32_t esClip = 0, esFarSmp = 0;   // Nova konuşurken kırpılmış mikrofon örneği
// 'a' ölçümünün kaydı: mikrofon yolundan (yankı silmeden önce) ham örnekler
static int16_t *volatile capBuf = nullptr;
static volatile int capWant = 0, capPos = 0;
static volatile uint32_t capStart = 0;
static volatile int32_t capOff = 0;

static void *alignedAlloc(size_t n) {
  void *p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) memset(p, 0, n);
  return p;
}

// AEC kurar. Çalışma belleği ECHO_AEC_PSRAM'e göre PSRAM'den ya da iç RAM'den. İç RAM'e yalnızca en büyük
// boş blok genişse geçilir: parçalı iç RAM'de ESP-SR'nin bellek alamayıp çökmesi riskine girilmez.
static aec_handle_t *aecMake(int mode, int nlp, int &chunk) {
  aec_config_t cfg = {};
  cfg.mic_num = 1;
  cfg.ref_num = 1;
  cfg.out_num = 1;
  cfg.filter_length = ECHO_FILTER_LEN;
  cfg.sample_rate = 16000;
  cfg.mode = (aec_mode_t)mode;
  cfg.nlp_level = (aec_nlp_level_t)nlp;
  bool bigInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= 96 * 1024;
  aec_handle_t *h = nullptr;
  if (!ECHO_AEC_PSRAM && bigInternal) {
    cfg.caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    h = aec_create_from_config(&cfg);
  }
  if (!h) {
    cfg.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    h = aec_create_from_config(&cfg);
  }
  chunk = h ? aec_get_chunksize(h) : 0;
  if (h && (chunk <= 0 || chunk > 1024)) { aec_destroy(h); h = nullptr; chunk = 0; }
  return h;
}

static void aecFreeBufs() {
  if (aecIn) heap_caps_free(aecIn);
  if (aecRef) heap_caps_free(aecRef);
  if (aecOut) heap_caps_free(aecOut);
  aecIn = aecRef = aecOut = nullptr;
}

// Mikrofon görevinden çağrılır: yankı silmeyi kurar / kaldırır
static void echoApply(bool on) {
  if (!on) {
    if (aecH) aec_destroy(aecH);
    aecH = nullptr;
    aecFreeBufs();
    aecOn = false;
    return;
  }
  if (echoDelayS < 0 || !refRing) {
    Log.println("[yankı] ölçülmemiş: yankı silme açılmadı ('a' ile ölç)");
    aecWant = false;
    return;
  }
  size_t inBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (aecFirst)
    Log.printf("[yankı] yankı silme kuruluyor (iç RAM boş %u KB, en büyük blok %u KB)\n", (unsigned)(inBefore / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  int64_t t0 = esp_timer_get_time();
  int nlp = echoNlpSel;
  aecLinear = nlp == 3;
  aecH = aecMake(ECHO_AEC_MODE, aecLinear ? 0 : nlp, aecC);
  if (aecH) {
    aecIn = (int16_t *)alignedAlloc(aecC * 2);
    aecRef = (int16_t *)alignedAlloc(aecC * 2);
    aecOut = (int16_t *)alignedAlloc(aecC * 2);
  }
  if (!aecH || !aecIn || !aecRef || !aecOut) {
    Log.printf("[yankı] HATA: yankı silme kurulamadı (iç RAM boş %u KB)\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    if (aecH) aec_destroy(aecH);
    aecH = nullptr;
    aecFreeBufs();
    aecWant = false;
    return;
  }
  if (!lbBuf && ECHO_LOOKBACK_MS > 0 && aecC <= 512) lbBuf = (int16_t *)psAlloc((size_t)LB_N * 512 * 2);
  lbHead = lbClosed = lbEmit = 0;
  lbWasOpen = true;
  duckUntil = 0;
  aecPos = 0;
  gate.set(echoResid[nlp & 3], ECHO_GATE_MARGIN_DB, (ECHO_GATE_HANG_MS * 16 + aecC - 1) / aecC);
  gate.duckFrac = powf(10.0f, -ECHO_DUCK_NEAR_DB / 20.0f);
  gate.nearNeed = ECHO_DUCK_NEAR_N;
  // 'a' ölçümü durağan hışırtıyla -42 dB verir ama konuşmada artık yankı -20 dB civarındadır (2.8.4 kaydı).
  // İlk oturumun ilk cümlesinde kapının kendi sesiyle açılmaması için canlı tahmin -26 dB tabanından başlar.
  if (gate.liveN == 0 && gate.liveDb < -26.0f) gate.liveDb = -26.0f;
  gate.hang = 0;
  aecOn = true;
  if (aecFirst) {
    aecFirst = false;
    Log.printf("[yankı] yankı silme kuruldu (NLP %s): parça %d örnek, %u ms, iç RAM %ld KB, gecikme %.1f ms\n", echoNlpName(nlp), aecC,
               (unsigned)((esp_timer_get_time() - t0) / 1000), ((long)inBefore - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024,
               echoDelayS / 16.0f);
  }
}

// Bir AEC parçası: hizalanmış referansı al, yankıyı sil, kapıya sor
static void echoChunk() {
  uint32_t start = aecStartIdx + (uint32_t)refOff - (uint32_t)echoDelayS - (uint32_t)ECHO_ADVANCE;
  uint32_t tot = refTotal;
  int32_t ahead = (int32_t)(tot - (start + (uint32_t)aecC));      // bu parçanın sonu yazılmış mı
  int32_t age = (int32_t)(tot - start);                           // halkadan silinmemiş mi
  if (ahead >= 0 && age < (int32_t)(REF_N - 4096)) {
    for (int i = 0; i < aecC; i++) aecRef[i] = refRing[(start + (uint32_t)i) & (REF_N - 1)];
  } else {
    memset(aecRef, 0, aecC * 2);
    esMiss = esMiss + 1;
  }
  int64_t t0 = esp_timer_get_time();
  if (aecLinear) aec_linear_process(aecH, aecIn, aecRef, aecOut);
  else aec_process(aecH, aecIn, aecRef, aecOut);
  uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  float refRms = echomath::rmsOf(aecRef, aecC), outRms = echomath::rmsOf(aecOut, aecC);
  bool far = refRms > gate.farMin;
  bool open = gate.decide(refRms, outRms);
  esChunks = esChunks + 1;
  esProcUs = esProcUs + us;
  if (us > esProcMax) esProcMax = us;
  if (far) {
    esFar = esFar + 1;
    uint32_t clip = 0;
    for (int i = 0; i < aecC; i++) if (aecIn[i] >= 32000 || aecIn[i] <= -32000) clip++;
    esClip = esClip + clip;
    esFarSmp = esFarSmp + (uint32_t)aecC;
    esInDb = esInDb + echomath::dbOf(echomath::rmsOf(aecIn, aecC));
    esOutDb = esOutDb + echomath::dbOf(outRms);
    if (open) esOpenFar = esOpenFar + 1;
    else { esResidSum = esResidSum + echomath::dbOf(outRms) - echomath::dbOf(refRms); esResidN = esResidN + 1; }
  }
  // Kısma: aday ses görülünce. Kapı hiç açılmadan biten kısma "boşa" sayılır (yankı tetiklemiş olabilir).
  {
    static bool duckEp = false, duckEpOpen = false;
    bool duckActive = (int32_t)(duckUntil - millis()) > 0;
    if (duckEp && !duckActive) {
      if (!duckEpOpen) esDuckWaste = esDuckWaste + 1;
      duckEp = false;
    }
    if (ECHO_DUCK_DB > 0 && gate.candidate) {
      if (!duckActive) { esDucks = esDucks + 1; duckEp = true; duckEpOpen = false; }
      duckUntil = millis() + ECHO_DUCK_HOLD_MS;
    }
    if (duckEp && open && gate.refEnv > gate.farMin) duckEpOpen = true;
  }
  // geriye dönük: bu parçanın ham hali saklanır; kapalı-açık geçişinde önceki kapalı parçalar gönderilecek
  if (lbBuf && LB_N > 0 && aecC <= 512) {
    memcpy(lbBuf + lbHead * aecC, aecOut, aecC * 2);
    lbHead = (lbHead + 1) % LB_N;
    bool uzak = gate.refEnv > gate.farMin;      // kapının kendi ölçüsü (referansın zarfı)
    if (open && !lbWasOpen && uzak && lbClosed > 0) {
      lbEmit = lbClosed < LB_N - 1 ? lbClosed : LB_N - 1;
      esLookback = esLookback + 1;
    }
    lbClosed = (!open && uzak) ? lbClosed + 1 : 0;
    lbWasOpen = open;
  }
  if (!open) memset(aecOut, 0, aecC * 2);
}

static void micTask(void *) {
  int32_t *const raw = micRaw;                  // 20 ms stereo (PSRAM)
  static LiveMon live;
  int16_t *const frame = micFrame;
  uint8_t *const dropBuf = micDrop;
  size_t pos = 0;
  HighPass hpL, hpR;                            // kanal başına
  static Upsampler2x up2;                       // yarı hızda çalışırken 8 -> 16 kHz
  int curRate = 0;                              // süzgeçlerin kurulduğu hız (0: henüz kurulmadı)
  uint64_t sumL = 0, sumR = 0;
  uint32_t cnt = 0;

  micBusy = true;
  micSelfTest();
  micBusy = false;

  while (running) {
    if (micRetestReq) {                         // Serial'den "m": testi baştan çalıştır
      micRetestReq = false;
      micBusy = true;
      Log.println("[mik] test yeniden başlıyor");
      micSetPins(PIN_MIC_SCK, PIN_MIC_WS, PIN_MIC_SD);
      micSelfTest();
      micBusy = false;
      outage = false;
      liveReset(live);
    }
    if (micCfgReq) {                            // hız + sürüş gücü, saat yeniden başlatılarak
      micCfgReq = false;
      int hz = micCfgHz;
      micDrive = micCfgDrive;
      micRestartClock(200);
      if (hz != micRate) micSetClock(hz);
      outage = false;
      liveReset(live);
    }
    if (curRate != micRate) {                   // hız değişti: süzgeçleri yeni hıza göre kur
      curRate = micRate;
      hpL.design(MIC_HPF_HZ, curRate);
      hpR.design(MIC_HPF_HZ, curRate);
      up2.reset();
      liveReset(live);
      sumL = sumR = 0;
      cnt = 0;
    }
    size_t got = 0;
    if (i2s_channel_read(rxChan, raw, MIC_RAW_N * sizeof(int32_t), &got, pdMS_TO_TICKS(200)) != ESP_OK || got == 0) continue;
    int64_t readUs = esp_timer_get_time();
    size_t frames = got / 8;
    uint16_t peak = 0;
    bool half = curRate == MIC_RATE / 2;
    bool pass = half || curRate == MIC_RATE;    // ses yolu yalnızca 16 kHz ve 8 kHz'de geçerli
    if (pass && refRing) {
      // Okumanın bittiği anda referans ve mikrofon sayaçları arasındaki fark; son 15 ölçümün ortancası
      int64_t pUs;
      uint32_t pTot;
      if (refStamp(pUs, pTot)) {
        uint32_t micNext = micTotal + (uint32_t)(half ? frames * 2 : frames);
        int32_t off = (int32_t)(pTot + (uint32_t)((readUs - pUs) * 16 / 1000) - micNext);
        offHist[offPos] = off;
        offPos = (offPos + 1) % 15;
        if (offN < 15) offN++;
        int32_t s[15];
        for (int k = 0; k < offN; k++) s[k] = offHist[k];
        for (int a = 1; a < offN; a++) { int32_t x = s[a]; int b = a - 1; while (b >= 0 && s[b] > x) { s[b + 1] = s[b]; b--; } s[b + 1] = x; }
        refOff = s[offN / 2];
        refOffOk = offN >= 5;
      }
    }
    if (aecWant != aecOn) echoApply(aecWant);
    uint32_t winLen = (uint32_t)curRate / 4, probeSkip = (uint32_t)curRate / 16;
    for (size_t i = 0; i < frames; i++) {
      for (int c = 0; c < 2; c++) {             // canlı izleme: 24 bit ham örnek üzerinden
        int32_t v = raw[2 * i + c] >> 8;
        if (v != live.prev[c]) live.chg[c]++;
        if (v >= (1 << 22) || v <= -(1 << 22)) live.big[c]++;
        if (v < live.mn[c]) live.mn[c] = v;
        if (v > live.mx[c]) live.mx[c] = v;
        live.prev[c] = v;
        // Yoklama penceresi: ilk 60 ms atlanır (çekme açılmadan önce tamponlanmış örnekler).
        if (live.probe && live.n >= probeSkip) {
          int ones = __builtin_popcount((uint32_t)raw[2 * i + c] & 0xFFFFFF00u);
          if (ones <= 2) live.pz[c]++;
          else if (ones >= 18) live.pf[c]++;
          if (c == 1) live.pn++;
        }
      }
      if (++live.n >= winLen) liveWindow(live);
      if (!pass) continue;
      int32_t l = raw[2 * i] >> MIC_SHIFT;
      int32_t r = raw[2 * i + 1] >> MIC_SHIFT;
      float yl = hpL.run((float)l);
      float yr = hpR.run((float)r);
      sumL += (uint64_t)fabsf(yl);
      sumR += (uint64_t)fabsf(yr);
      cnt++;
      int use = micUse;
      float mix = use == 1 ? yl : (use == 2 ? yr : (yl + yr) * 0.5f);
      float outv[2];
      int outn = 1;
      if (half) { up2.push(mix, outv[0], outv[1]); outn = 2; }    // 8 kHz -> 16 kHz
      else outv[0] = mix;
      for (int q = 0; q < outn; q++) {
        float m = outv[q];
        int16_t v = m > 32767.0f ? 32767 : (m < -32768.0f ? -32768 : (int16_t)lroundf(m));
        uint16_t a = (uint16_t)(v < 0 ? -(int32_t)v : v);
        if (a > peak) peak = a;
        uint32_t idx = micTotal;
        micTotal = idx + 1;
        int16_t va = sat16(m * ECHO_IN_GAIN);   // yankı silmenin girişi: kırpılmasın diye kısılmış
        if (capBuf && capPos < capWant) {        // 'a' ölçümünün kaydı (yankı silmeye verilecek ölçekte)
          if (capPos == 0) { capStart = idx; capOff = refOff; }
          capBuf[capPos] = va;
          capPos = capPos + 1;
        }
        // Yankı silme açıksa örnekler AEC parçası kadar biriktirilir, temizlenmiş hali çerçeveye gider
        const int16_t *src = &v;
        int srcN = 1;
        if (aecH) {
          if (aecPos == 0) aecStartIdx = idx;
          aecIn[aecPos++] = va;
          if (aecPos < aecC) continue;
          aecPos = 0;
          echoChunk();
          const float up = 1.0f / ECHO_IN_GAIN;      // girişteki kısma geri alınır
          for (int k = 0; k < aecC; k++) aecOut[k] = sat16(aecOut[k] * up);
          src = aecOut;
          srcN = aecC;
          // Kapı şimdi açıldı: önce kapalıyken sıfırlanmış son parçalar (sözün başı) gönderilir
          for (; lbEmit > 0; lbEmit--) {
            const int16_t *eski = lbBuf + ((lbHead - 1 - lbEmit + 2 * LB_N) % LB_N) * aecC;
            for (int k = 0; k < aecC; k++) {
              frame[pos++] = sat16(eski[k] * up);
              if (pos == MIC_FRAME_SAMPLES) {
                pos = 0;
                if (xQueueSend(micQ, frame, 0) != pdTRUE) {
                  xQueueReceive(micQ, dropBuf, 0);
                  xQueueSend(micQ, frame, 0);
                }
              }
            }
          }
        }
        for (int k = 0; k < srcN; k++) {
          frame[pos++] = src[k];
          if (pos == MIC_FRAME_SAMPLES) {
            pos = 0;
            if (xQueueSend(micQ, frame, 0) != pdTRUE) {
              xQueueReceive(micQ, dropBuf, 0);      // kuyruk dolu: en eskiyi at
              xQueueSend(micQ, frame, 0);
            }
          }
        }
      }
    }
    lvlMic = peak;
    if (!pass) { pos = 0; continue; }
    if (cnt >= (uint32_t)curRate) {             // saniyede bir kanal ortalamaları
      uint64_t aL = sumL / cnt, aR = sumR / cnt;
      lvlMicL = (uint16_t)(aL > 32767 ? 32767 : aL);
      lvlMicR = (uint16_t)(aR > 32767 ? 32767 : aR);
      sumL = 0;
      sumR = 0;
      cnt = 0;
    }
  }
  vTaskDelete(nullptr);
}

bool micReadFrame(uint8_t *out) {
  if (!micQ) return false;
  return xQueueReceive(micQ, out, 0) == pdTRUE;
}

// ---------- yankı silme: dışarıya açılan kısım ----------
void echoSetCalibration(int delaySamples, const float residByNlp[4]) {
  echoDelayS = delaySamples;
  for (int i = 0; i < 4; i++) echoResid[i] = residByNlp[i];
}
void echoSetNlp(int nlp) { echoNlpSel = (nlp == 1 || nlp == 3) ? nlp : 0; }
int echoGetNlp() { return echoNlpSel; }
const char *echoNlpName(int nlp) { return nlp == 1 ? "güçlü" : nlp == 3 ? "kapalı (yalnız doğrusal)" : "normal"; }
int echoDelaySamples() { return echoDelayS; }
void micSetEchoCancel(bool on) { aecWant = on; }
bool micEchoActive() { return aecOn; }

size_t echoStatusText(char *b, size_t cap) {
  if (!cap) return 0;
  b[0] = 0;
  if (!aecOn) return 0;
  uint32_t ch = esChunks, far = esFar, open = esOpenFar, miss = esMiss, us = esProcUs, mx = esProcMax;
  uint32_t clip = esClip, farSmp = esFarSmp;
  float inDb = esInDb, outDb = esOutDb;
  esChunks = 0; esFar = 0; esOpenFar = 0; esMiss = 0; esProcUs = 0; esProcMax = 0; esResidN = 0;
  esResidSum = 0; esInDb = 0; esOutDb = 0; esClip = 0; esFarSmp = 0;
  int n = snprintf(b, cap, "yankı (NLP %s): işlem %.1f ms (en çok %.1f) / %d ms", echoNlpName(aecLinear ? 3 : echoNlpSel),
                   ch ? us / 1000.0f / ch : 0.0f, mx / 1000.0f, aecC / 16);
  if (far && n > 0 && (size_t)n < cap)
    n += snprintf(b + n, cap - n, " | Nova konuşurken: mikrofon %.0f dBFS, silme sonrası %.0f dBFS (bastırma %.0f dB), kapı açık %%%u, kırpılan %%%.1f",
                  inDb / far + ECHO_HEAD_DB, outDb / far + ECHO_HEAD_DB, (inDb - outDb) / far, (unsigned)(100 * open / far),
                  farSmp ? 100.0f * clip / farSmp : 0.0f);
  if (far && n > 0 && (size_t)n < cap)       // kapının eşiği (artık / referans): ölçümden ve oturumdaki canlı orandan
    n += snprintf(b + n, cap - n, ", eşik %.0f dB (ölçüm %.0f, canlı %.0f)", gate.thresholdDb(), gate.baseDb, gate.liveDb);
  uint32_t dk = esDucks, dw = esDuckWaste, lb = esLookback;
  esDucks = 0; esDuckWaste = 0; esLookback = 0;
  if ((dk || dw || lb) && n > 0 && (size_t)n < cap)
    n += snprintf(b + n, cap - n, ", kısma %u (boşa %u), geriye dönük %u", (unsigned)dk, (unsigned)dw, (unsigned)lb);
  if (miss && n > 0 && (size_t)n < cap) n += snprintf(b + n, cap - n, ", referanssız %u", (unsigned)miss);
  return n > 0 ? (size_t)n : 0;
}

static const char *aecName(int mode, int nlp) {
  static char b[48];
  snprintf(b, sizeof(b), "%s + NLP %s", mode == 5 ? "FD düşük maliyet" : mode == 6 ? "FD yüksek başarım" : "diğer",
           echoNlpName(nlp));
  return b;
}

// 'a': yankı ölçümü. Oturum kapalıyken çağrılır (ana döngüden; ~6 sn sürer). 0,5 sn sessizlik (ortam),
// ardından 3 sn hışırtı çalınır; mikrofon ve referans birlikte kaydedilir. Gecikme çapraz ilintiyle
// bulunur, sonra kayıt yankı silmeden kartın üstünde (çevrimdışı) geçirilip ne kadar bastırdığı ölçülür.
static bool echoMeasureImpl(EchoCal &r) {
  memset(&r, 0, sizeof(r));
  r.delay = -1;
  if (!running || !refRing || aecOn) { snprintf(r.err, sizeof(r.err), "ses yolu hazır değil"); return false; }
  const int AMB = 8000, NOISE = 48000, TOTAL = AMB + NOISE, DMAX = ECHO_DELAY_MAX;
  int16_t *cap = (int16_t *)psAlloc(TOTAL * 2);
  int16_t *ref = (int16_t *)psAlloc((TOTAL + DMAX) * 2);
  int16_t *noise = (int16_t *)psAlloc(2400 * 2);
  if (!cap || !ref || !noise) {
    free(cap); free(ref); free(noise);
    snprintf(r.err, sizeof(r.err), "bellek yok");
    return false;
  }
  // Her adım kayda yazılır: kart çökerse yeniden açılışta [önceki] satırlarında nerede kaldığı görünür.
  // 1) sessizlikte kayıt başlar; 0,5 sn sonra hışırtı (3,2 sn, beyaz gürültü ~ -27 dBFS)
  Log.println("[yankı] 1/5 kayıt başlıyor");
  spkFlush();
  vTaskDelay(pdMS_TO_TICKS(300));
  capPos = 0;
  capWant = TOTAL;
  capBuf = cap;
  uint32_t t0 = millis();
  while (capPos < AMB && millis() - t0 < 2000) vTaskDelay(pdMS_TO_TICKS(5));
  uint32_t seed = 0x1234567u;
  for (int blk = 0; blk < 32; blk++) {
    for (int i = 0; i < 2400; i++) {
      seed = seed * 1664525u + 1013904223u;
      noise[i] = (int16_t)((int32_t)(seed >> 16) % 3000 - 1500) * 2;
    }
    spkWrite((const uint8_t *)noise, 2400 * 2);
  }
  spkMarkEnd();
  while (capPos < capWant && millis() - t0 < 8000) vTaskDelay(pdMS_TO_TICKS(10));
  bool full = capPos >= capWant;
  capBuf = nullptr;
  uint32_t cs = capStart;
  int32_t co = capOff;
  // 2) referansın aynı aralığını halkadan al (mikrofon örneği k, gecikme d: ref[k + DMAX - d])
  uint32_t rbase = cs + (uint32_t)co - (uint32_t)DMAX;
  uint32_t tot = refTotal;
  bool refOk = full && refOffOk && (int32_t)(tot - (rbase + TOTAL + DMAX)) >= 0 && (int32_t)(tot - rbase) < (int32_t)REF_N;
  if (refOk)
    for (int j = 0; j < TOTAL + DMAX; j++) ref[j] = refRing[(rbase + (uint32_t)j) & (REF_N - 1)];
  free(noise);
  if (!refOk) {
    snprintf(r.err, sizeof(r.err), full ? "referans hizalanamadı" : "mikrofon kaydı tamamlanmadı");
    free(cap); free(ref);
    return false;
  }
  Log.println("[yankı] 2/5 kayıt tamam, gecikme aranıyor");
  r.ambientDb = echomath::dbOf(echomath::rmsOf(cap + 800, AMB - 1600)) + ECHO_HEAD_DB;
  r.echoDb = echomath::dbOf(echomath::rmsOf(cap + AMB + 8000, NOISE - 8000)) + ECHO_HEAD_DB;
  // 3) gecikme: hışırtının ortasından 1 sn'lik pencere
  const int W = 16384, s0 = AMB + 12000;
  echomath::LagResult lr = echomath::findLag(cap + s0, ref + s0, W, DMAX, [] { vTaskDelay(1); });
  r.delay = lr.lag;
  r.quality = lr.quality;
  if (lr.lag < 0 || lr.quality < 8.0f || lr.lag < ECHO_ADVANCE + 16 || lr.lag > DMAX - 16) {
    snprintf(r.err, sizeof(r.err), "yankı bulunamadı (güven %.1f): ses çok kısık ya da hoparlör kapalı olabilir", lr.quality);
    free(cap); free(ref);
    return false;
  }
  // 4) kayıt yankı silmeden geçirilir: üç NLP ayarı (güçlü, normal, kapalı = yalnızca doğrusal)
  const int nlps[3] = {1, 0, 3};
  for (int t = 0; t < 3; t++) {
    EchoCal::Run &run = r.run[t];
    run.mode = ECHO_AEC_MODE;
    run.nlp = nlps[t];
    Log.printf("[yankı] %d/5 gecikme %.1f ms; yankı silme deneniyor (%s, iç RAM boş %u KB, en büyük blok %u KB)\n", 3 + t,
               r.delay / 16.0f, aecName(run.mode, run.nlp), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    size_t inBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int C = 0;
    aec_handle_t *h = aecMake(run.mode, run.nlp == 3 ? 0 : run.nlp, C);
    int16_t *bi = h ? (int16_t *)alignedAlloc(C * 2) : nullptr;
    int16_t *br = h ? (int16_t *)alignedAlloc(C * 2) : nullptr;
    int16_t *bo = h ? (int16_t *)alignedAlloc(C * 2) : nullptr;
    run.ramKb = h ? (int)(((long)inBefore - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024) : 0;
    Log.printf("[yankı]     %s (parça %d örnek), kayıt işleniyor\n", h ? "kuruldu" : "KURULAMADI", C);
    if (h && bi && br && bo) {
      double mic2 = 0, out2 = 0, ref2 = 0;
      long nm = 0;
      uint32_t usSum = 0, usMax = 0, nch = 0;
      int from = AMB + 2000, measFrom = AMB + NOISE / 2;
      for (int s = from; s + C <= TOTAL; s += C) {
        memcpy(bi, cap + s, C * 2);
        memcpy(br, ref + s + DMAX - (r.delay - ECHO_ADVANCE), C * 2);
        int64_t a = esp_timer_get_time();
        if (run.nlp == 3) aec_linear_process(h, bi, br, bo);
        else aec_process(h, bi, br, bo);
        uint32_t us = (uint32_t)(esp_timer_get_time() - a);
        usSum += us;
        if (us > usMax) usMax = us;
        nch++;
        if (s >= measFrom) {
          for (int i = 0; i < C; i++) { mic2 += (double)bi[i] * bi[i]; out2 += (double)bo[i] * bo[i]; ref2 += (double)br[i] * br[i]; }
          nm += C;
        }
        if ((nch & 7) == 7) vTaskDelay(1);
      }
      if (nm) {
        float micDb = echomath::dbOf((float)sqrt(mic2 / nm)), outDb = echomath::dbOf((float)sqrt(out2 / nm));
        float refDb = echomath::dbOf((float)sqrt(ref2 / nm));
        run.ok = true;
        run.outDb = outDb + ECHO_HEAD_DB;
        run.erleDb = micDb - outDb;
        run.residRefDb = outDb - refDb;
        run.cpuMs = nch ? usSum / 1000.0f / nch : 0;
        run.cpuMaxMs = usMax / 1000.0f;
        run.chunkMs = C / 16.0f;
      }
    }
    if (bi) heap_caps_free(bi);
    if (br) heap_caps_free(br);
    if (bo) heap_caps_free(bo);
    if (h) aec_destroy(h);
  }
  free(cap);
  free(ref);
  r.ok = r.run[0].ok && r.run[1].ok && r.run[2].ok;
  if (!r.ok) snprintf(r.err, sizeof(r.err), "yankı silme kurulamadı");
  return r.ok;
}

// Ölçüm kendi görevinde koşar: ESP-SR'nin yankı silmesi Arduino döngüsünün 8 KB'lık yığınına sığmayabilir.
static EchoCal *measOut = nullptr;
static volatile bool measDone = false, measOk = false;
static void echoMeasureTask(void *) {
  measOk = echoMeasureImpl(*measOut);
  measDone = true;
  vTaskDelete(nullptr);
}
bool echoMeasure(EchoCal &r) {
  memset(&r, 0, sizeof(r));
  measOut = &r;
  measDone = false;
  measOk = false;
  if (xTaskCreatePinnedToCore(echoMeasureTask, "nova_echo", 16384, nullptr, 2, nullptr, 1) != pdPASS) {
    snprintf(r.err, sizeof(r.err), "ölçüm görevi kurulamadı (bellek)");
    return false;
  }
  uint32_t t0 = millis();
  while (!measDone && millis() - t0 < 60000) vTaskDelay(pdMS_TO_TICKS(20));
  if (!measDone) { snprintf(r.err, sizeof(r.err), "ölçüm bitmedi"); return false; }
  return measOk;
}

const char *echoRunName(const EchoCal::Run &run) { return aecName(run.mode, run.nlp); }

// I2S çıkış sinyalini ikinci bir pine de kopyalar (sol amfi için)
static void mirrorOutput(int pin, uint32_t signal) {
  gpio_reset_pin((gpio_num_t)pin);
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
  esp_rom_gpio_connect_out_signal(pin, signal, false, false);
}

// Kapatılan tarafın veri pini 0'a çekilir: saatler akmaya devam eder, ses gitmez.
void audioRoute(bool left, bool right) {
  uint32_t sig = i2s_periph_signal[I2S_NUM_0].data_out_sig;
  esp_rom_gpio_connect_out_signal(PIN_AMP_L_DIN, left ? sig : SIG_GPIO_OUT_IDX, false, false);
  if (!left) gpio_set_level((gpio_num_t)PIN_AMP_L_DIN, 0);
  esp_rom_gpio_connect_out_signal(PIN_AMP_R_DIN, right ? sig : SIG_GPIO_OUT_IDX, false, false);
  if (!right) gpio_set_level((gpio_num_t)PIN_AMP_R_DIN, 0);
}

bool audioBegin() {
  // ----- çalma tamponu -----
  if (!ringMem) {
    bool ps = psramFound();
    ringCap = (size_t)SPK_RATE * 2 * (ps ? SPK_RING_MS : SPK_RING_MS_NOPSRAM) / 1000;
    ringMem = (uint8_t *)(ps ? ps_malloc(ringCap) : malloc(ringCap));
    if (!ringMem) { Log.println("[ses] tampon ayrılamadı"); return false; }
    Log.printf("[ses] çalma tamponu %u ms (%s)\n", (unsigned)(ringCap / 2 * 1000 / SPK_RATE), ps ? "PSRAM" : "heap");
  }
  engine.init(ringMem, ringCap, SPK_BLOCK, (size_t)SPK_RATE * 2 * SPK_PREBUF_MS / 1000);
  loud.setup((float)SPK_RATE, SPK_HPF_HZ, SPK_BOOST_DB, powf(10.0f, SPK_CEILING_DB / 20.0f), 3.0f, SPK_LIMIT_REL_MS);
  spkSetVolume(volumePct);
  Log.printf("[ses] ses ayarı %%%d, gürlük artırma +%.0f dB (sınırlayıcı tavanı %.0f dB)\n", (int)volumePct, (double)SPK_BOOST_DB,
             (double)SPK_CEILING_DB);

  // ----- amfiler: I2S0 TX -----
  i2s_chan_config_t txc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  txc.dma_desc_num = 8;
  txc.dma_frame_num = SPK_BLOCK;
  txc.auto_clear = true;
  if (i2s_new_channel(&txc, &txChan, nullptr) != ESP_OK) return false;

  i2s_std_config_t tx = {};
  tx.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE);
  tx.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  tx.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  tx.gpio_cfg.bclk = (gpio_num_t)PIN_AMP_R_BCLK;
  tx.gpio_cfg.ws = (gpio_num_t)PIN_AMP_R_LRC;
  tx.gpio_cfg.dout = (gpio_num_t)PIN_AMP_R_DIN;
  tx.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(txChan, &tx) != ESP_OK) return false;

  // Aynı üç sinyal sol amfinin pinlerine de gitsin
  mirrorOutput(PIN_AMP_L_BCLK, i2s_periph_signal[I2S_NUM_0].m_tx_bck_sig);
  mirrorOutput(PIN_AMP_L_LRC,  i2s_periph_signal[I2S_NUM_0].m_tx_ws_sig);
  mirrorOutput(PIN_AMP_L_DIN,  i2s_periph_signal[I2S_NUM_0].data_out_sig);

  // ----- mikrofonlar: I2S1 RX -----
  i2s_chan_config_t rxc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  rxc.dma_desc_num = 6;
  rxc.dma_frame_num = 320;
  if (i2s_new_channel(&rxc, nullptr, &rxChan) != ESP_OK) return false;

  i2s_std_config_t rx = {};
  rx.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE);
  rx.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  rx.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  rx.gpio_cfg.bclk = (gpio_num_t)PIN_MIC_SCK;
  rx.gpio_cfg.ws = (gpio_num_t)PIN_MIC_WS;
  rx.gpio_cfg.dout = I2S_GPIO_UNUSED;
  rx.gpio_cfg.din = (gpio_num_t)PIN_MIC_SD;
  if (i2s_channel_init_std_mode(rxChan, &rx) != ESP_OK) return false;

  if (i2s_channel_enable(txChan) != ESP_OK) return false;
  if (i2s_channel_enable(rxChan) != ESP_OK) return false;
  applyMicDrive(PIN_MIC_SCK, PIN_MIC_WS);
  rxOn = true;

  // Çalışma tamponları ve mikrofon kuyruğu (6 x 2,5 KB) PSRAM'de; kuyruk alınamazsa iç RAM'de
  psLazy(refRing, REF_N);                       // yankı silmenin referans halkası (128 KB, PSRAM)
  psLazy(playMono, SPK_BLOCK);
  psLazy(playStereo, SPK_BLOCK * 2);
  psLazy(micRaw, MIC_RAW_N);
  psLazy(micFrame, MIC_FRAME_SAMPLES);
  psLazy(micDrop, MIC_FRAME_BYTES);
  if (!playMono || !playStereo || !micRaw || !micFrame || !micDrop) return false;
  if (!micQ) micQ = xQueueCreateWithCaps(6, MIC_FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!micQ) micQ = xQueueCreate(6, MIC_FRAME_BYTES);
  if (!micQ) return false;
  running = true;
  xTaskCreatePinnedToCore(playTask, "nova_play", 8192, nullptr, 6, nullptr, 1);   // seviye satırı ondalıklı yazdırıyor: yığın geniş
  xTaskCreatePinnedToCore(micTask, "nova_mic", 16384, nullptr, 5, nullptr, 1);   // yankı silme (ESP-SR) yığın ister
  return true;
}

void audioStop() {
  if (!running) return;
  running = false;
  for (int i = 0; i < 100 && micBusy; i++) vTaskDelay(pdMS_TO_TICKS(100));   // öz-test sürüyorsa bitsin
  vTaskDelay(pdMS_TO_TICKS(300));               // görevler kendi kendine çıksın
  if (txChan) { i2s_channel_disable(txChan); i2s_del_channel(txChan); txChan = nullptr; }
  if (rxChan) { rxDisable(); i2s_del_channel(rxChan); rxChan = nullptr; }
}
