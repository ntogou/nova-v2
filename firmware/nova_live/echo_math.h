// Yankı silme (Aşama 3) için donanımdan bağımsız hesaplar. Masaüstünde de sınanır.
//
//   rmsOf / dbOf      : seviye (32767 = tam ölçek, 0 dBFS)
//   findLag           : hoparlöre giden sesin mikrofona kaç örnek sonra ulaştığı (çapraz ilinti)
//   EchoGate          : Nova konuşurken mikrofon sesini modele verip vermeme kararı
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

namespace echomath {

static inline float rmsOf(const int16_t *x, size_t n) {
  if (!n) return 0;
  double s = 0;
  for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
  return (float)sqrt(s / n);
}
static inline float dbOf(float rms) { return rms > 0.5f ? 20.0f * log10f(rms / 32767.0f) : -96.0f; }

struct LagResult {
  int lag = -1;          // örnek (16 kHz)
  float quality = 0;     // tepe / ortalama |ilinti|; 8'in üstü güvenilir
};

// mic[0..n) ile ref arasındaki gecikmeyi bulur. ref, mic'ten dMax örnek önce başlar:
// mic[k] için aday gecikme d'nin referansı ref[k + dMax - d] (0 <= d <= dMax).
// Önce dörde indirilmiş sinyalle kaba arama, sonra tam hızda ±6 örnek.
// yield() uzun döngüde ara sıra çağrılır (kartta bekçi köpeği için).
template <typename Yield>
LagResult findLag(const int16_t *mic, const int16_t *ref, int n, int dMax, Yield yield) {
  LagResult r;
  const int nd = n / 4, dd = dMax / 4, rd = nd + dd;
  float *m4 = new float[nd], *r4 = new float[rd];
  for (int i = 0; i < nd; i++) m4[i] = (float)mic[4 * i] + mic[4 * i + 1] + mic[4 * i + 2] + mic[4 * i + 3];
  for (int i = 0; i < rd; i++) r4[i] = (float)ref[4 * i] + ref[4 * i + 1] + ref[4 * i + 2] + ref[4 * i + 3];
  float best = 0;
  int bestC = 0;
  double sumAbs = 0;
  for (int c = 0; c <= dd; c++) {
    const float *rp = r4 + dd - c;
    float s = 0;
    for (int i = 0; i < nd; i++) s += m4[i] * rp[i];
    float a = fabsf(s);
    sumAbs += a;
    if (a > best) { best = a; bestC = c; }
    if ((c & 63) == 63) yield();
  }
  delete[] m4;
  delete[] r4;
  r.quality = (float)(best / (sumAbs / (dd + 1) + 1e-9));
  int lo = bestC * 4 - 6, hi = bestC * 4 + 6;
  if (lo < 0) lo = 0;
  if (hi > dMax) hi = dMax;
  int64_t bestF = -1;
  for (int d = lo; d <= hi; d++) {
    const int16_t *rp = ref + dMax - d;
    int64_t s = 0;
    for (int k = 0; k < n; k++) s += (int32_t)mic[k] * rp[k];
    if (s < 0) s = -s;
    if (s > bestF) { bestF = s; r.lag = d; }
  }
  yield();
  return r;
}

// Nova konuşurken (referans sesliyken) yankı silmenin çıkışı, artık yankının belirgin üstündeyse
// "kullanıcı konuşuyor" sayılır; değilse modele sessizlik gider. Referans sessizken kapı hep açıktır.
// Bir kez açılınca kısa bir süre açık kalır (hece aralarında kesilmesin).
//
// Eşik iki kaynaktan büyük olanı + pay:
//   - 'a' ölçümündeki oran (hışırtıyla; 32-35 dB bastırma verir)
//   - oturumda canlı ölçülen oran: Nova konuşurken çıkış/referans oranının %80'lik dilimi. Konuşmada
//     bastırma 12-17 dB'e düşüyor (2.4.3 ve 2.8.0 kayıtları); yalnız ölçüme bakan eşik her parçada
//     delindiği için kapı %90-100 açık kalıyor, model kendi sesinin artığını duyuyordu.
// Referansın kendisi değil yavaş sönen zarfı kullanılır: hece bitince referans düşer ama odadaki yankı
// bir süre daha gelir; anlık referansla oran fırlar ve kapı açılırdı.
struct EchoGate {
  float baseDb = -20;        // ölçülen artık / referans (dB)
  float liveDb = -20;        // canlı ölçülen oranın %80'lik dilimi (dB)
  float marginDb = 8;
  float farMin = 60;         // referans zarfı bunun altındaysa Nova susuyor sayılır (~-55 dBFS)
  float nearFloor = 40;      // Nova konuşurken kullanıcı sesi için en düşük seviye (yankı silme girişinin ölçeğinde)
  float envDecay = 0.8f;     // zarf parça başına bu oranla söner (32 ms'de -1,9 dB)
  float refEnv = 0;
  int hangChunks = 8;        // açıldıktan sonra açık kalma (parça)
  int hang = 0;
  int hit = 0;               // eşiğin hemen üstündeki ardışık parça sayısı (tek parçalık patlamalı harf yankısını eler)
  uint32_t liveN = 0;        // canlı orana katılan parça sayısı (açılıştan beri)
  void set(float residRefDb, float margin, int hangN) {
    if (residRefDb != baseDb || !liveN) liveDb = residRefDb;   // ölçüm değiştiyse canlı tahmin baştan
    baseDb = residRefDb;
    marginDb = margin;
    hangChunks = hangN;
    refEnv = 0;
    hang = 0;
    hit = 0;
  }
  float thresholdDb() const { return (liveDb > baseDb ? liveDb : baseDb) + marginDb; }
  // refRms: bu parçanın hizalanmış referansı, outRms: yankı silmenin çıkışı
  bool decide(float refRms, float outRms) {
    refEnv = refRms > refEnv * envDecay ? refRms : refEnv * envDecay;
    bool far = refEnv > farMin;
    if (!far) { hang = 0; hit = 0; return true; }
    float ratioDb = dbOf(outRms) - dbOf(refEnv);
    // Canlı oran: yalnızca Nova bu parçada gerçekten ses çıkarırken (hece/kelime arası sönme kuyruğunda
    // referans 0 iken güncellenirse eşik hızla düşer ve sonraki "B/P/K" harfinde kendi yankısıyla açılır).
    if (refRms > farMin * 4 && refRms >= refEnv * 0.6f) {
      liveDb += ratioDb > liveDb ? 0.4f : -0.05f;
      if (liveDb < baseDb) liveDb = baseDb;
      if (liveDb > baseDb + 30) liveDb = baseDb + 30;
      liveN++;
    }
    float thr = refEnv * powf(10.0f, thresholdDb() / 20.0f);
    if (thr < nearFloor) thr = nearFloor;
    if (outRms > thr) {
      if (hang > 0 || outRms > thr * 1.35f || ++hit >= 2) { hang = hangChunks; return true; }
      return false;
    }
    hit = 0;
    if (hang > 0) { hang--; return true; }
    return false;
  }
};

}  // namespace echomath
