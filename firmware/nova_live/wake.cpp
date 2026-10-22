#include "wake.h"
#include <Arduino.h>
#include "netlog.h"
#include <string.h>
#include "esp_heap_caps.h"
extern "C" {
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
extern const uint8_t heynova_model[];
extern const size_t heynova_model_len;
}

namespace {

srmodel_list_t *models = nullptr;
char *modelName = nullptr;
const esp_wn_iface_t *wn = nullptr;
model_iface_data_t *wd = nullptr;
int chunk = 0;                     // motorun bir seferde istediği örnek sayısı
int16_t *stage = nullptr;          // mikrofon çerçevesi (80 ms) motorun parçasına (32 ms) bölünür
int staged = 0;
volatile int mode = WAKE_SENSITIVE;
bool firstArm = true;

}  // namespace

const char *wakeModeName(int m) {
  return m == WAKE_OFF ? "kapalı" : m == WAKE_NORMAL ? "normal" : m == WAKE_VERY ? "çok hassas" : "hassas";
}
void wakeSetMode(int m) { mode = (m < WAKE_OFF || m > WAKE_VERY) ? WAKE_SENSITIVE : m; }
int wakeMode() { return mode; }

bool wakeBegin() {
  models = srmodel_load(heynova_model);
  modelName = models ? esp_srmodel_filter(models, ESP_WN_PREFIX, NULL) : nullptr;
  if (!modelName) {
    Log.println("[uyan] HATA: uyandırma modeli okunamadı; uyandırma sözü kapalı (BOOT ile uyandır)");
    return false;
  }
  wn = esp_wn_handle_from_name(modelName);
  if (!wn) {
    Log.printf("[uyan] HATA: '%s' modeli için motor bulunamadı; uyandırma sözü kapalı\n", modelName);
    return false;
  }
  char *words = esp_srmodel_get_wake_words(models, modelName);
  Log.printf("[uyan] model: %s (%s), %u KB, firmware'in içinde\n", modelName, words ? words : "?",
                (unsigned)(heynova_model_len / 1024));
  return true;
}

bool wakeArm(bool on) {
  if (!wn) return false;
  if (!on) {
    if (wd) {
      wn->destroy(wd);
      wd = nullptr;
    }
    if (stage) {
      heap_caps_free(stage);
      stage = nullptr;
    }
    return false;
  }
  if (wd) return true;
  if (mode == WAKE_OFF) return false;
  size_t inBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t psBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t t0 = millis();
  // DET_MODE_90: normal (WakeNet10'da ağırlıklar flaştan okunur); DET_MODE_95: hassas (PSRAM'e kopyalanır)
  wd = wn->create(modelName, mode == WAKE_NORMAL ? DET_MODE_90 : DET_MODE_95);
  if (!wd) {
    Log.println("[uyan] HATA: motor kurulamadı (bellek?)");
    return false;
  }
  // Çok hassas: modelin karar eşiği düşürülür (ESP-SR'nin izin verdiği en düşük değer 0,4)
  float defThr = wn->get_det_threshold(wd, 1), thr = defThr;
  if (mode == WAKE_VERY) {
    thr = defThr - WAKE_VERY_DROP;
    if (thr < 0.4f) thr = 0.4f;
    wn->set_det_threshold(wd, thr, 1);
  }
  static int loggedMode = -1;
  if (loggedMode != mode) {
    loggedMode = mode;
    Log.printf("[uyan] karar eşiği %.2f (modun varsayılanı %.2f; düşük = daha çok yakalar)\n", thr, defThr);
  }
  chunk = wn->get_samp_chunksize(wd);
  stage = (int16_t *)heap_caps_malloc((size_t)chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!stage) stage = (int16_t *)heap_caps_malloc((size_t)chunk * sizeof(int16_t), MALLOC_CAP_8BIT);
  if (!stage) {
    wn->destroy(wd);
    wd = nullptr;
    Log.println("[uyan] HATA: tampon ayrılamadı");
    return false;
  }
  staged = 0;
  if (firstArm) {
    // İlk kurulumda maliyeti yaz: oturum sırasında iç RAM dar (61-68 KB boş), bu yüzden motor uykuda kurulur
    firstArm = false;
    long inUsed = (long)inBefore - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    long psUsed = (long)psBefore - (long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Log.printf("[uyan] dinlemede (%s): parça %d örnek @ %d Hz, kurulum %u ms, iç RAM %ld KB, PSRAM %ld KB\n",
                  wakeModeName(mode), chunk, wn->get_samp_rate(wd), (unsigned)(millis() - t0), inUsed / 1024, psUsed / 1024);
  }
  return true;
}

bool wakeFeed(const int16_t *pcm, size_t n) {
  if (!wd || !stage) return false;
  while (n) {
    size_t take = (size_t)(chunk - staged);
    if (take > n) take = n;
    memcpy(stage + staged, pcm, take * sizeof(int16_t));
    staged += (int)take;
    pcm += take;
    n -= take;
    if (staged == chunk) {
      staged = 0;
      if (wn->detect(wd, stage) == WAKENET_DETECTED) {
        wn->clean(wd);              // aynı söz ikinci kez sayılmasın
        return true;
      }
    }
  }
  return false;
}
