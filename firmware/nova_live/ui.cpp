// Nova V2 — ekran arayüzü (160x128 yatay, A · Bakır Çekirdek)
//
// Çizim ekran.cpp'de ve donanımdan bağımsız. Burası her karede veriyi toplar (durum, ses seviyeleri,
// yazılar, saat, pil), kareyi PSRAM'deki tampona çizdirir, bir önceki kareyle karşılaştırır ve
// yalnızca değişen piksel aralıklarını ekrana yollar. Uykuda kare hızı düşer.
#include "ui.h"
#include "audio.h"
#include "ekran.h"
#include "hafiza.h"
#include "psbuf.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define SCR_W ekran::GEN
#define SCR_H ekran::YUK
#define NOVA_MAX 640          // Nova'nın altyazısı (bayt, UTF-8)
#define KUL_MAX 240           // kullanıcının son sözü
#define NOT_MAX 120           // kısa not ("Mikrofon tamam.")
#define NOT_MS 8000           // not uykuda ve dinlerken bu kadar görünür
#define ETKINLIK_MS 3000      // araç etiketi bu kadar görünür
#define UYKU_FPS 5
#define BOSLUK 12             // satırdaki iki değişik parça arası bundan kısaysa tek parça yollanır

static Adafruit_ST7735 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
static uint16_t *kare = nullptr, *onceki = nullptr;     // SCR_W*SCR_H, PSRAM
static SemaphoreHandle_t mux = nullptr;

static volatile UiState state = UI_BOOT;
static volatile int brightness = DEFAULT_BRIGHTNESS;
static volatile int otaPct = 0;
static volatile bool wifiUp = false;
static volatile int wifiRssi = -100;
static volatile bool bootIpucu = true;     // uyandırma motoru kurulana kadar BOOT
static volatile uint32_t arkaBas = 0;

// mux ile korunan yazılar (büyükleri PSRAM'de: iç RAM şifreli bağlantıya kalsın)
static char *novaBuf = nullptr;        // NOVA_MAX + 1
static int novaVurgu = -1;             // son eklenen parçanın başı (kehribar)
static bool novaSistem = false;        // tampondaki yazı bir sistem notu: Nova'nın sözü gelince silinir
static char *kulBuf = nullptr;         // KUL_MAX + 1
static bool sonKullanici = false;      // en son gelen yazı kullanıcıdan
static uint32_t sonKulMs = 0;          // son kullanıcı yazısının geldiği an (gecikmeli ekleri aynı cümlede tutmak için)
static char notBuf[NOT_MAX] = "";
static uint32_t notAt = 0;
static char havaBuf[40] = "";
static char etkinlikBuf[40] = "";
static uint32_t etkinlikAt = 0;
static char *gecici = nullptr;         // GECICI_MAX: temizlenen yazı (mux altında)
static char *aNova = nullptr, *aKul = nullptr;   // ekran görevinin kopyaları
#define GECICI_MAX 1024

static volatile float batV = 0.0f;
static volatile bool batLow = false;

// kayıt için ölçümler (üstel ortalama)
static volatile float olcCiz = 0, olcGonder = 0, olcPiksel = 0, olcFps = 0;

// ---------- dışarıya açık ----------
static volatile bool ledOverride = false;
static void setStateLed(UiState s);
void uiSetState(UiState s) { state = s; }
// Durum LED'ini geçici olarak başka bir işe verir (mikrofon arıza bulma kipi).
void uiLedOverride(int r, int g, int b) {
#if PIN_RGB_LED >= 0
  ledOverride = true;
  rgbLedWrite(PIN_RGB_LED, r, g, b);
#endif
}
void uiLedRelease() {
  ledOverride = false;
  setStateLed(state);
}
UiState uiGetState() { return state; }
void uiSetOta(int percent) { otaPct = percent; }
void uiSetBootHint(bool on) { bootIpucu = on; }
void uiSetLink(bool up, int rssi) { wifiUp = up; wifiRssi = rssi; }
void uiSetArkaModel(uint32_t basladiMs) { arkaBas = basladiMs; }

static void kopyala(char *dst, size_t cap, const char *src) {
  size_t n = src ? strlen(src) : 0;
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;   // UTF-8 harfi ortadan bölme
  }
  if (n) memcpy(dst, src, n);
  dst[n] = 0;
}

void uiSetHava(const char *kisa) {
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  kopyala(havaBuf, sizeof(havaBuf), kisa);
  xSemaphoreGive(mux);
}

void uiSetEtkinlik(const char *etiket) {
  if (!mux) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  kopyala(etkinlikBuf, sizeof(etkinlikBuf), etiket);
  etkinlikAt = millis() | 1;
  xSemaphoreGive(mux);
}

static void applyBacklight(int percent) {
  percent = constrain(percent, 0, 100);
  // Göze doğrusal gelsin diye karesel eğri
  uint32_t duty = (uint32_t)percent * percent * 255 / 10000;
  if (percent > 0 && duty < 6) duty = 6;
  ledcWrite(PIN_TFT_BLK, duty);
}

void uiSetBrightness(int percent) {
  brightness = constrain(percent, 0, 100);
  applyBacklight(brightness);
}
int uiGetBrightness() { return brightness; }
void uiScreenOff() { applyBacklight(0); }

float batVolts() { return batV; }
bool batIsLow() { return batLow; }

int batPercent() {
  float v = batV;
  if (v < BAT_VALID_MIN_V) return -1;
  // Li-ion boşalma eğrisi (yaklaşık)
  static const float tv[] = {3.20f, 3.50f, 3.62f, 3.70f, 3.75f, 3.80f, 3.85f, 3.92f, 4.00f, 4.10f, 4.20f};
  static const int   tp[] = {0,     10,    20,    30,    40,    50,    60,    70,    80,    90,    100};
  if (v <= tv[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (v <= tv[i]) return tp[i - 1] + (int)((v - tv[i - 1]) / (tv[i] - tv[i - 1]) * (tp[i] - tp[i - 1]));
  }
  return 100;
}

// ---------- yazılar ----------
void uiText(const char *who, const char *text, bool fresh) {
  if (!mux || !text || !novaBuf) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  uint32_t simdi = millis();
  bool kullanici = who && strcmp(who, "user") == 0;
  if (kullanici) {
    // Nova konuşmaya başladıktan hemen sonra gelen gecikmeli hece ("Sel" + "am") önceki sözün devamıdır: silinmez
    if (fresh || (!sonKullanici && (uint32_t)(simdi - sonKulMs) > 2500)) kulBuf[0] = 0;
    ekran::ekle(kulBuf, KUL_MAX, text, nullptr, gecici, GECICI_MAX);
    sonKulMs = simdi;
    if (state != UI_SPEAK) sonKullanici = true;
  } else {
    if (fresh || sonKullanici || novaSistem) { novaBuf[0] = 0; novaVurgu = -1; }   // yeni cevap
    ekran::ekle(novaBuf, NOVA_MAX, text, &novaVurgu, gecici, GECICI_MAX);
    novaSistem = fresh;
    if (fresh) {
      ekran::temizle(text, notBuf, sizeof(notBuf), true);
      notAt = simdi | 1;
    }
    sonKullanici = false;
  }
  xSemaphoreGive(mux);
}

void uiClearText() {
  if (!mux || !novaBuf) return;
  xSemaphoreTake(mux, portMAX_DELAY);
  novaBuf[0] = 0;
  novaVurgu = -1;
  novaSistem = false;
  kulBuf[0] = 0;
  sonKullanici = false;
  sonKulMs = 0;
  notAt = 0;
  xSemaphoreGive(mux);
}

// ---------- çizim ----------
static float levelNorm(uint16_t peak) {
  // 200 -> 0, 2000 -> 0.5, 20000 -> 1
  float l = (log10f(1.0f + peak) - 2.3f) / 2.0f;
  return l < 0 ? 0 : (l > 1 ? 1 : l);
}

static ekran::Durum ekranDurumu(UiState s) {
  switch (s) {
    case UI_BOOT:   return ekran::ACILIS;
    case UI_WIFI:   return ekran::WIFI;
    case UI_SERVER: return ekran::BAGLANIYOR;
    case UI_IDLE:   return ekran::UYKU;
    case UI_LISTEN: return ekran::DINLIYOR;
    case UI_THINK:  return ekran::DUSUNUYOR;
    case UI_SPEAK:  return ekran::KONUSUYOR;
    case UI_OTA:    return ekran::GUNCELLEME;
    case UI_LOWBAT: return ekran::PIL_BITTI;
  }
  return ekran::UYKU;
}

// Yeni kareyi bir öncekiyle karşılaştırır, değişen aralıkları yollar. Gönderilen piksel sayısını döner.
static uint32_t gonder(const uint16_t *yeni, const uint16_t *eski, bool hepsi) {
  uint32_t toplam = 0;
  bool acik = false;
  for (int y = 0; y < SCR_H; y++) {
    const uint16_t *a = yeni + y * SCR_W, *b = eski + y * SCR_W;
    int x = 0;
    while (x < SCR_W) {
      if (!hepsi && a[x] == b[x]) { x++; continue; }
      int bas = x, son = x;
      for (int i = x + 1; i < SCR_W && i - son <= BOSLUK; i++)
        if (hepsi || a[i] != b[i]) son = i;
      int w = son - bas + 1;
      if (!acik) { tft.startWrite(); acik = true; }
      tft.setAddrWindow(bas, y, w, 1);
      tft.writePixels((uint16_t *)a + bas, w, true, false);
      toplam += w;
      x = son + 1;
    }
  }
  if (acik) tft.endWrite();
  return toplam;
}

static void setStateLed(UiState s) {
#if PIN_RGB_LED >= 0
  switch (s) {
    case UI_LISTEN: rgbLedWrite(PIN_RGB_LED, 0, 20, 0); break;
    case UI_THINK:  rgbLedWrite(PIN_RGB_LED, 20, 10, 0); break;
    case UI_SPEAK:  rgbLedWrite(PIN_RGB_LED, 0, 6, 20); break;
    case UI_LOWBAT: rgbLedWrite(PIN_RGB_LED, 20, 0, 0); break;
    case UI_WIFI:
    case UI_SERVER: rgbLedWrite(PIN_RGB_LED, 6, 0, 6); break;
    default:        rgbLedWrite(PIN_RGB_LED, 0, 0, 0); break;
  }
#endif
}

static void readBattery() {
  static bool first = true;
  static uint32_t lowSince = 0;
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BAT_ADC);
  float v = (mv / 8) / 1000.0f * BAT_DIVIDER * BAT_CAL;
  if (first) { batV = v; first = false; }
  else batV = batV * 0.8f + v * 0.2f;

  float f = batV;
  if (f >= BAT_VALID_MIN_V && f < BAT_LOW_V) {
    if (lowSince == 0) lowSince = millis();
    if (millis() - lowSince > 20000) batLow = true;
  } else {
    lowSince = 0;
  }
}

size_t uiStatsText(char *out, size_t cap) {
  int n = snprintf(out, cap, "çizim %.1f ms, gönderim %.1f ms, %u piksel/kare, %.1f kare/sn",
                   olcCiz, olcGonder, (unsigned)olcPiksel, olcFps);
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void uiTask(void *) {
  char aNot[NOT_MAX], aHava[40], aEtkinlik[40];
  uint32_t sonPil = 0, sonKare = 0;
  UiState lastState = (UiState)255;
  float mik = 0, ses = 0;
  bool ilk = true;

  for (;;) {
    uint32_t now = millis();
    if (sonPil == 0 || now - sonPil > 2000) { sonPil = now; readBattery(); }

    UiState s = state;
    if (s != lastState) { lastState = s; if (!ledOverride) setStateLed(s); }

    // ses seviyeleri: yükselişte hemen, düşüşte yavaş
    float hm = s == UI_LISTEN ? levelNorm(micLevel()) : 0;
    mik = hm > mik ? hm : mik * 0.82f;
    float hs = s == UI_SPEAK ? levelNorm(spkLevel()) : 0;
    ses = hs > ses ? hs : ses * 0.85f;

    ekran::Veri v;
    v.durum = ekranDurumu(s);
    v.t = (now % 3600000u) / 1000.0f;            // saatte bir başa döner (float hassasiyeti)
    v.mik = mik;
    v.ses = ses;
    v.pil = batPercent();
    v.wifi = wifiUp;
    v.rssi = wifiRssi;
    v.ota = constrain((int)otaPct, 0, 100);
    v.bootIpucu = bootIpucu;

    int64_t t = (int64_t)time(nullptr);
    if (t > 1700000000) {
      int y, a, g, sa, d, hg;
      hafiza::yerelTakvim(t, &y, &a, &g, &sa, &d, &hg);
      v.saat = sa; v.dakika = d; v.gun = g; v.ay = a; v.haftaGunu = hg;
    }

    xSemaphoreTake(mux, portMAX_DELAY);
    strcpy(aNova, novaBuf);
    v.vurguBas = novaVurgu;
    // dinlerken kullanıcının sözü yalnızca Nova'nın son sözünden yeniyse görünür
    strcpy(aKul, (s == UI_LISTEN && !sonKullanici) ? "" : kulBuf);
    bool notVar = notAt && (s == UI_BOOT || now - notAt < NOT_MS);
    strcpy(aNot, notVar ? notBuf : "");
    strcpy(aHava, havaBuf);
    strcpy(aEtkinlik, (etkinlikAt && now - etkinlikAt < ETKINLIK_MS) ? etkinlikBuf : "");
    xSemaphoreGive(mux);

    uint32_t arka = arkaBas;
    if (arka) {
      v.etkinlikSn = (int)((now - arka) / 1000);
      if (!aEtkinlik[0]) strcpy(aEtkinlik, "araştırıyor…");
    }
    v.altyazi = aNova;
    v.kullanici = aKul;
    v.bildirim = aNot;
    v.hava = aHava;
    v.etkinlik = aEtkinlik;

    uint32_t t0 = micros();
    ekran::ciz(kare, v);
    uint32_t t1 = micros();
    uint32_t px = gonder(kare, onceki, ilk);
    uint32_t t2 = micros();
    ilk = false;
    uint16_t *tak = onceki; onceki = kare; kare = tak;   // gönderilen kare artık "önceki"

    olcCiz = olcCiz * 0.95f + (t1 - t0) / 1000.0f * 0.05f;
    olcGonder = olcGonder * 0.95f + (t2 - t1) / 1000.0f * 0.05f;
    olcPiksel = olcPiksel * 0.95f + px * 0.05f;
    uint32_t dt = now - sonKare;
    if (sonKare && dt) olcFps = olcFps * 0.95f + (1000.0f / dt) * 0.05f;
    sonKare = now;

    int fps = (s == UI_IDLE) ? UYKU_FPS : (s == UI_LOWBAT ? 2 : UI_FPS);
    uint32_t periyot = 1000 / fps, gecen = millis() - now;
    vTaskDelay(pdMS_TO_TICKS(gecen + 2 < periyot ? periyot - gecen : 2));   // boşta görev de çalışsın
  }
}

void uiBegin() {
  novaBuf = (char *)psAlloc(NOVA_MAX + 1);
  kulBuf = (char *)psAlloc(KUL_MAX + 1);
  gecici = (char *)psAlloc(GECICI_MAX);
  aNova = (char *)psAlloc(NOVA_MAX + 1);
  aKul = (char *)psAlloc(KUL_MAX + 1);
  bool yaziVar = novaBuf && kulBuf && gecici && aNova && aKul;
  if (!yaziVar) { free(novaBuf); novaBuf = nullptr; }   // uiText bunu görünce hiçbir şey yapmaz
  mux = xSemaphoreCreateMutex();
  ledcAttach(PIN_TFT_BLK, 5000, 8);
  applyBacklight(0);

  SPI.begin(PIN_TFT_SCK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.initR(TFT_TAB);
  tft.setSPISpeed(TFT_SPI_HZ);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ekran::rgb(0x0B, 0x07, 0x05));
  analogSetPinAttenuation(PIN_BAT_ADC, ADC_11db);

  size_t boy = (size_t)SCR_W * SCR_H * sizeof(uint16_t);
  kare = (uint16_t *)heap_caps_malloc(boy, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  onceki = (uint16_t *)heap_caps_malloc(boy, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!kare || !onceki) {                       // PSRAM yoksa iç bellekten dene
    if (!kare) kare = (uint16_t *)malloc(boy);
    if (!onceki) onceki = (uint16_t *)malloc(boy);
  }
  if (!kare || !onceki || !yaziVar) {
    tft.setTextColor(0xF800);
    tft.setCursor(4, 60);
    tft.print("ekran bellegi yok");
    applyBacklight(brightness);
    return;
  }

  applyBacklight(brightness);
  xTaskCreatePinnedToCore(uiTask, "nova_ui", 8192, nullptr, 1, nullptr, 0);
}
