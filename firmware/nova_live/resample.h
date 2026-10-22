// 16 kHz -> 24 kHz örnekleme çevirici (3/2 oranı, çok fazlı FIR).
// OpenAI Realtime 24 kHz ses ister; mikrofonlar (ve ileride yankı önleme) 16 kHz çalışır.
// Donanıma bağlı değil.
//
// Yöntem: araya sıfır koyarak 48 kHz'e çık, 8 kHz'in üstünü süz, iki örnekte birini al.
// Süzgeç 96 katsayılı Kaiser pencereli sinc; her çıkış örneği için 32 çarpma yapılır.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

class Upsampler16to24 {
 public:
  static const int kTapsPerPhase = 32;
  static const int kTaps = kTapsPerPhase * 3;

  Upsampler16to24() { reset(); }
  void reset() { memset(hist_, 0, sizeof(hist_)); }

  // n çift olmalı. out en az n*3/2 örnek alır. Yazılan örnek sayısını döner.
  size_t process(const int16_t *in, size_t n, int16_t *out) {
    const float *h = table();
    const int H = kTapsPerPhase - 1;
    size_t outN = n * 3 / 2;
    for (size_t m = 0; m < outN; m++) {
      size_t up = 2 * m;                 // 48 kHz'deki konum
      int phase = (int)(up % 3);
      long idx = (long)(up / 3);         // karşılık gelen giriş örneği
      const float *hp = h + phase * kTapsPerPhase;
      float acc = 0;
      for (int k = 0; k < kTapsPerPhase; k++) {
        long j = idx - k;
        float x = j >= 0 ? (float)in[j] : (float)hist_[H + j];     // j < 0: önceki çağrıdan kalan örnekler
        acc += hp[k] * x;
      }
      int32_t v = (int32_t)lrintf(acc);
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      out[m] = (int16_t)v;
    }
    if (n >= (size_t)H) {
      memcpy(hist_, in + n - H, H * sizeof(int16_t));
    } else {
      memmove(hist_, hist_ + n, (H - n) * sizeof(int16_t));
      memcpy(hist_ + H - n, in, n * sizeof(int16_t));
    }
    return outN;
  }

 private:
  int16_t hist_[kTapsPerPhase - 1];

  static float bessel0(float x) {
    float sum = 1, term = 1;
    for (int k = 1; k < 25; k++) {
      term *= (x / (2 * k)) * (x / (2 * k));
      sum += term;
    }
    return sum;
  }

  // Katsayılar ilk kullanımda bir kez hesaplanır: tbl[faz][k] = h[faz + 3k]
  static const float *table() {
    static float tbl[3 * kTapsPerPhase];
    static bool ready = false;
    if (!ready) {
      const float beta = 7.0f;                       // ~70 dB bastırma
      const float fc = 7800.0f / 48000.0f;           // kesim (48 kHz'e göre)
      const float mid = (kTaps - 1) / 2.0f;
      const float pi = 3.14159265358979f;
      float proto[kTaps];
      for (int i = 0; i < kTaps; i++) {
        float t = i - mid;
        float s = fabsf(t) < 1e-6f ? 2 * fc : sinf(2 * pi * fc * t) / (pi * t);
        float r = t / mid;
        float w = bessel0(beta * sqrtf(1 - r * r > 0 ? 1 - r * r : 0)) / bessel0(beta);
        proto[i] = 3.0f * s * w;                     // araya sıfır koymanın kaybını kazanç 3 ile geri al
      }
      // Her fazın toplamı tam 1 olsun (sabit girişte dalgalanma kalmasın)
      for (int p = 0; p < 3; p++) {
        float sum = 0;
        for (int k = 0; k < kTapsPerPhase; k++) sum += proto[p + 3 * k];
        for (int k = 0; k < kTapsPerPhase; k++) tbl[p * kTapsPerPhase + k] = proto[p + 3 * k] / sum;
      }
      ready = true;
    }
    return tbl;
  }
};

// 2 kat örnekleme yükseltici (8 kHz -> 16 kHz), örnek örnek çalışır.
// Mikrofonlar yarı hızda (8 kHz) çalıştırıldığında sesi 16 kHz'e tamamlamak için.
// 32 katsayılı Kaiser pencereli sinc; 3,7 kHz'in üstü süzülür (telefon bandı).
class Upsampler2x {
 public:
  static const int kTapsPerPhase = 16;
  Upsampler2x() { reset(); }
  void reset() { memset(hist_, 0, sizeof(hist_)); pos_ = 0; }

  // Bir giriş örneği alır, iki çıkış örneği verir.
  void push(float x, float &y0, float &y1) {
    const float *h = table();
    hist_[pos_] = x;
    float a = 0, b = 0;
    int j = pos_;
    for (int k = 0; k < kTapsPerPhase; k++) {
      float v = hist_[j];
      a += h[k] * v;                              // faz 0
      b += h[kTapsPerPhase + k] * v;              // faz 1
      j = j == 0 ? kTapsPerPhase - 1 : j - 1;
    }
    pos_ = pos_ + 1 == kTapsPerPhase ? 0 : pos_ + 1;
    y0 = a;
    y1 = b;
  }

 private:
  float hist_[kTapsPerPhase];
  int pos_;

  static float bessel0(float x) {
    float sum = 1, term = 1;
    for (int k = 1; k < 25; k++) {
      term *= (x / (2 * k)) * (x / (2 * k));
      sum += term;
    }
    return sum;
  }

  static const float *table() {
    static float tbl[2 * kTapsPerPhase];
    static bool ready = false;
    if (!ready) {
      const int N = 2 * kTapsPerPhase;
      const float beta = 6.0f, fc = 3700.0f / 16000.0f, mid = (N - 1) / 2.0f, pi = 3.14159265358979f;
      float proto[2 * kTapsPerPhase];
      for (int i = 0; i < N; i++) {
        float t = i - mid;
        float sv = fabsf(t) < 1e-6f ? 2 * fc : sinf(2 * pi * fc * t) / (pi * t);
        float r = t / mid;
        float w = bessel0(beta * sqrtf(1 - r * r > 0 ? 1 - r * r : 0)) / bessel0(beta);
        proto[i] = 2.0f * sv * w;
      }
      for (int p = 0; p < 2; p++) {
        float sum = 0;
        for (int k = 0; k < kTapsPerPhase; k++) sum += proto[p + 2 * k];
        for (int k = 0; k < kTapsPerPhase; k++) tbl[p * kTapsPerPhase + k] = proto[p + 2 * k] / sum;
      }
      ready = true;
    }
    return tbl;
  }
};

// 24 kHz -> 16 kHz indirici (2/3 oranı). Yankı silmenin referansı için: hoparlöre giden ses 16 kHz'e
// indirilip mikrofonla karşılaştırılır. Araya sıfır koyarak 48 kHz'e çık, 7,4 kHz'in üstünü süz, üçte
// birini al. 96 katsayılı Kaiser pencereli sinc (Aşama 1'deki ölçüm düzeniyle aynı süzgeç).
// Blok boyu 3'ün katı ve en çok kMaxIn olmalı; n giriş için n*2/3 çıkış yazar.
class Downsampler24to16 {
 public:
  static const int kTaps = 96;
  static const int kHist = kTaps / 2;
  static const int kMaxIn = 480;
  Downsampler24to16() { reset(); }
  void reset() { memset(buf_, 0, sizeof(buf_)); }

  size_t process(const int16_t *in, size_t n, int16_t *out) {
    if (n > (size_t)kMaxIn || n % 3) return 0;
    const float *h = table();
    float *x = buf_ + kHist;                      // x[-kHist..-1]: önceki bloğun son örnekleri
    for (size_t i = 0; i < n; i++) x[i] = (float)in[i];
    size_t outN = n * 2 / 3;
    for (size_t j = 0; j < outN; j++) {
      long n48 = 3 * (long)j;                     // bu çıkışın 48 kHz'deki yeri (blok başına göre)
      float acc = 0;
      for (int k = (int)(n48 & 1); k < kTaps; k += 2) acc += h[k] * x[(n48 - k) / 2];
      int32_t v = (int32_t)lrintf(acc);
      if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
      out[j] = (int16_t)v;
    }
    memmove(buf_, buf_ + n, kHist * sizeof(float));
    return outN;
  }

 private:
  float buf_[kHist + kMaxIn];

  static float bessel0(float x) {
    float sum = 1, term = 1;
    for (int k = 1; k < 25; k++) {
      term *= (x / (2 * k)) * (x / (2 * k));
      sum += term;
    }
    return sum;
  }
  static const float *table() {
    static float h[kTaps];
    static bool ready = false;
    if (!ready) {
      const float beta = 7.0f, fc = 7400.0f / 48000.0f, mid = (kTaps - 1) / 2.0f, pi = 3.14159265358979f;
      float sum[2] = {0, 0};
      for (int i = 0; i < kTaps; i++) {
        float t = i - mid;
        float s = fabsf(t) < 1e-6f ? 2 * fc : sinf(2 * pi * fc * t) / (pi * t);
        float r = t / mid;
        h[i] = s * bessel0(beta * sqrtf(1 - r * r > 0 ? 1 - r * r : 0)) / bessel0(beta);
        sum[i & 1] += h[i];
      }
      for (int i = 0; i < kTaps; i++) h[i] /= sum[i & 1];      // iki fazın da düz kazancı 1
      ready = true;
    }
    return h;
  }
};
