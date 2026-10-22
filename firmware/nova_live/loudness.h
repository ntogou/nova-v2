// Hoparlör yolu için gürlük artırma. Donanıma bağlı değil.
//
// Modelin konuşmasında tepe noktaları yüksek, ortalama düşüktür. Ses ayarı tepeye göre
// yapılınca konuşma, hoparlörün verebileceğinin epey altında kalır. Burada yapılan:
//   1) Yüksek geçiren süzgeç: 5 cm'lik hoparlörün zaten veremediği en alt frekanslar atılır
//      (amfinin gücü duyulmayan yere gitmesin).
//   2) Sabit kazanç (boostDb): bütün ses yükseltilir.
//   3) Sınırlayıcı: tavanı aşacak tepeler, ses oraya gelmeden kısa süre önce yumuşakça
//      kısılır (ileri bakışlı); tepe geçince kazanç yavaşça geri açılır. Kırpma yapmaz,
//      dalga biçimini bozmaz; yalnızca yüksek anlarda sesi kısar.
//   4) Ses ayarı (volume): en sonda uygulanır. Yani ses ayarı yüzde kaçsa tepe de odur.
// Gecikme: lookMs (3 ms).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

class Loudness {
 public:
  static const int kMaxLook = 128;                 // 24 kHz'de 5 ms'ye kadar

  Loudness() { setup(24000.0f, 150.0f, 0.0f, 1.0f); }

  // ceiling: sınırlayıcının tavanı (0..1; 1 = tam ölçek). boostDb 0 ise yalnızca süzgeç ve tavan çalışır.
  void setup(float fs, float hpfHz, float boostDb, float ceiling, float lookMs = 3.0f, float releaseMs = 120.0f) {
    fs_ = fs;
    boost_ = powf(10.0f, boostDb / 20.0f);
    ceil_ = ceiling < 0.05f ? 0.05f : (ceiling > 1.0f ? 1.0f : ceiling);
    look_ = (int)(lookMs * fs / 1000.0f + 0.5f);
    if (look_ < 1) look_ = 1;
    if (look_ > kMaxLook) look_ = kMaxLook;
    rel_ = expf(-1.0f / (releaseMs * 0.001f * fs));
    // Kazanç yumuşatması: ileri bakış süresinin altıda biri. Tepe geldiğinde kazanç hedefe varmış olur.
    atk_ = 1.0f - expf(-1.0f / ((float)look_ / 6.0f));
    // 2. derece Butterworth yüksek geçiren
    float w0 = 2.0f * (float)M_PI * hpfHz / fs;
    float c = cosf(w0), alpha = sinf(w0) * 0.70710678f, a0 = 1.0f + alpha;
    b0_ = (1.0f + c) * 0.5f / a0;
    b1_ = -(1.0f + c) / a0;
    b2_ = b0_;
    a1_ = -2.0f * c / a0;
    a2_ = (1.0f - alpha) / a0;
    reset();
  }

  void reset() {
    memset(dly_, 0, sizeof(dly_));
    pos_ = 0;
    s1_ = s2_ = 0;
    env_ = 0;
    hold_ = 0;
    gain_ = 1.0f;
    clearStats();
  }

  void setVolume(float v) { vol_ = v < 0 ? 0 : (v > 1.0f ? 1.0f : v); }
  float boost() const { return boost_; }
  // Kazancı çalışırken değiştirir (sağlayıcıların ses seviyesi farklı). Süzgeç ve sınırlayıcı durumu korunur.
  void setBoostDb(float boostDb) { boost_ = powf(10.0f, boostDb / 20.0f); }
  float ceiling() const { return ceil_; }

  // Tek kanal, yerinde çalışabilir (in == out olabilir).
  void process(const int16_t *in, int16_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
      float x = (float)in[i] * (1.0f / 32768.0f);
      if (statsOn_) {
        float ax = fabsf(x);
        if (ax > stPeak_) stPeak_ = ax;
        stSq_ += (double)x * x;
        stN_++;
      }

      // 1) yüksek geçiren + 2) kazanç
      float y = b0_ * x + s1_;
      s1_ = b1_ * x - a1_ * y + s2_;
      s2_ = b2_ * x - a2_ * y;
      y *= boost_;

      // 3) sınırlayıcı: tepe izleyici (anında yükselir, ileri bakış süresince tutar, sonra bırakır)
      float a = fabsf(y);
      if (a >= env_) { env_ = a; hold_ = look_; }
      else if (hold_ > 0) hold_--;
      else env_ *= rel_;
      float target = env_ > ceil_ ? ceil_ / env_ : 1.0f;
      if (target < gain_) gain_ += (target - gain_) * atk_;      // kısarken hızlı
      else gain_ = target;                                       // açarken tepe izleyicinin bırakma hızı zaten yavaş
      if (statsOn_ && gain_ < stMinGain_) stMinGain_ = gain_;

      float d = dly_[pos_];                                      // look_ örnek önceki ses
      dly_[pos_] = y;
      pos_ = pos_ + 1 == look_ ? 0 : pos_ + 1;
      float o = d * gain_;
      if (o > ceil_) o = ceil_; else if (o < -ceil_) o = -ceil_;  // emniyet: kazanç tam yetişmediyse
      o *= vol_;

      int32_t v = (int32_t)lrintf(o * 32767.0f);
      out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
  }

  // Ölçüm (son clearStats'ten beri): gelen sesin tepesi ve ortalaması (dBFS), sınırlayıcının en çok kıstığı (dB, <= 0)
  uint32_t statSamples() const { return stN_; }
  float statPeakDb() const { return stPeak_ > 1e-6f ? 20.0f * log10f(stPeak_) : -120.0f; }
  float statRmsDb() const { return stN_ && stSq_ > 0 ? (float)(10.0 * log10(stSq_ / stN_)) : -120.0f; }
  float statLimitDb() const { return 20.0f * log10f(stMinGain_ > 1e-6f ? stMinGain_ : 1e-6f); }
  void clearStats() { stPeak_ = 0; stSq_ = 0; stN_ = 0; stMinGain_ = 1.0f; }
  void statsEnable(bool on) { statsOn_ = on; }      // sessiz bloklar ortalamayı düşürmesin diye

 private:
  float fs_ = 24000, boost_ = 1, ceil_ = 1, rel_ = 0.999f, atk_ = 0.1f, vol_ = 1.0f;
  int look_ = 72;
  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, s1_ = 0, s2_ = 0;
  float dly_[kMaxLook];
  int pos_ = 0;
  float env_ = 0, gain_ = 1.0f;
  int hold_ = 0;
  float stPeak_ = 0, stMinGain_ = 1.0f;
  double stSq_ = 0;
  uint32_t stN_ = 0;
  bool statsOn_ = true;
};
