// Nova V2 — çalma motoru
//
// Beyinden gelen 24 kHz mono sesi tamponlar ve 10 ms'lik bloklar halinde
// stereo olarak verir. Donanıma bağlı hiçbir şey içermez; bu sayede bilgisayarda
// da test edilebiliyor (tests/test_play_engine.cpp).
//
// Tek yazan (ağ döngüsü) ve tek okuyan (çalma görevi) varsayılır:
//   yazan:  write(), flush(), markEnd()
//   okuyan: renderBlock()
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

class PlayEngine {
 public:
  // buf: tampon belleği, cap: bayt (çift sayı), block: blok başına örnek,
  // prebufBytes: çalmaya başlamadan önce birikmesi gereken ses
  void init(uint8_t *buf, size_t cap, int block, size_t prebufBytes) {
    ring_ = buf; cap_ = cap & ~(size_t)1; block_ = block; prebuf_ = prebufBytes;
    head_ = 0; tail_ = 0; flushTo_ = 0;
    flushReq_ = false; endPending_ = false;
    playing_ = false; active_ = false;
    fadePos_ = 0; rampLeft_ = 0; lastOut_ = 0; lastDataMs_ = 0;
  }

  // ---- yazan taraf ----
  size_t write(const uint8_t *data, size_t len) {
    if (!ring_) return 0;
    len &= ~(size_t)1;
    // Kesme istenmişse eski ses yok sayılır: yeri hemen kullanılabilir.
    size_t h0 = head_, t0 = flushReq_ ? flushTo_ : tail_;
    size_t used = h0 >= t0 ? h0 - t0 : cap_ - t0 + h0;
    size_t freeBytes = (cap_ - 2) - used;
    if (len > freeBytes) { overflow_ = overflow_ + 1; len = freeBytes; }
    size_t h = head_;
    size_t first = len < cap_ - h ? len : cap_ - h;
    memcpy(ring_ + h, data, first);
    if (len > first) memcpy(ring_, data + first, len - first);
    head_ = (h + len) % cap_;
    return len;
  }
  // Çalmayı hemen keser. Bundan sonra yazılan ses korunur.
  void flush() { endPending_ = false; flushTo_ = head_; flushReq_ = true; }
  // "Bu cevabın sesi tamam": tampon boşalınca bitiş olayı üretilir.
  void markEnd() { endPending_ = true; }

  size_t count() const {
    size_t h = head_, t = tail_;
    return h >= t ? h - t : cap_ - t + h;
  }
  // Çalmaya başlamadan (ve her "ses yetişmedi"den sonra) biriktirilecek ses. Çalarken değiştirilebilir.
  void setPrebuf(size_t bytes) { prebuf_ = bytes & ~(size_t)1; }
  uint32_t overflowCount() const { return overflow_; }
  // Çalarken tamponun boşaldığı (ses yetişmediği) an sayısı. Sesi gerçek zamanlı yollayan
  // sağlayıcıda ağ gecikmesinin göstergesi; her cevabın doğal bitişi de bir kez sayılır.
  uint32_t underrunCount() const { return underrun_; }
  void setGainQ15(int32_t g) { gain_ = g; }

  // Olaylar: bir kez okunur
  bool takeStart() { if (evStart_) { evStart_ = false; return true; } return false; }
  bool takeEnd()   { if (evEnd_)   { evEnd_ = false;   return true; } return false; }

  // ---- okuyan taraf ----
  // stereoOut: block*2 örnek. Dönen değer: bloktaki tepe seviye.
  uint16_t renderBlock(int16_t *stereoOut, int16_t *monoScratch, uint32_t nowMs) {
    if (flushReq_) {
      tail_ = flushTo_;
      flushReq_ = false;
      playing_ = false;
      if (active_) { active_ = false; evEnd_ = true; }
    }

    // Önce bitiş işareti, sonra doluluk okunur: işaret ses yazıldıktan sonra
    // konduğu için bu sırayla okununca yarım kalan ses olmaz.
    bool ep = endPending_;
    size_t avail = count();
    if (ep && avail == 0) {
      endPending_ = false;
      playing_ = false;
      active_ = false;
      evEnd_ = true;
    } else if (!playing_) {
      if (avail >= prebuf_ || (ep && avail > 0)) {
        playing_ = true;
        fadePos_ = 0;                    // her yeniden başlayışta yumuşak giriş
        if (!active_) { active_ = true; evStart_ = true; }
      }
    }

    size_t n = 0;
    if (playing_ && avail >= 2) {
      size_t want = avail < (size_t)block_ * 2 ? avail : (size_t)block_ * 2;
      want &= ~(size_t)1;
      size_t t = tail_;
      size_t first = want < cap_ - t ? want : cap_ - t;
      memcpy((uint8_t *)monoScratch, ring_ + t, first);
      if (want > first) memcpy((uint8_t *)monoScratch + first, ring_, want - first);
      tail_ = (t + want) % cap_;
      n = want / 2;
      lastDataMs_ = nowMs;
    } else if (playing_) {
      playing_ = false;                 // ağ gecikti: yeniden biriktir
      underrun_ = underrun_ + 1;
    } else if (active_ && nowMs - lastDataMs_ > kStarveMs) {
      active_ = false;                  // cevap yarım kaldı, veri gelmiyor
      evEnd_ = true;
    }

    int32_t g = gain_;
    uint16_t peak = 0;
    for (int i = 0; i < block_; i++) {
      int32_t s = 0;
      if ((size_t)i < n) {
        s = ((int32_t)monoScratch[i] * g) >> 15;
        if (fadePos_ < kFade) { s = s * fadePos_ / kFade; fadePos_++; }   // yumuşak giriş
        rampLeft_ = kRamp;
        lastOut_ = sat16(s);
      } else if (rampLeft_ > 0) {
        rampLeft_--;
        s = (int32_t)lastOut_ * rampLeft_ / kRamp;                        // çıt sesi olmasın
      }
      int16_t v = sat16(s);
      uint16_t a = (uint16_t)(v < 0 ? -(int32_t)v : v);
      if (a > peak) peak = a;
      stereoOut[2 * i] = v;
      stereoOut[2 * i + 1] = v;
    }
    return peak;
  }

  static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

 private:
  static const int kFade = 240;        // 10 ms
  static const int kRamp = 96;         // 4 ms
  static const uint32_t kStarveMs = 1500;

  uint8_t *ring_ = nullptr;
  size_t cap_ = 0;
  volatile size_t prebuf_ = 0;
  int block_ = 0;
  volatile size_t head_ = 0, tail_ = 0, flushTo_ = 0;
  volatile bool flushReq_ = false, endPending_ = false;
  volatile bool evStart_ = false, evEnd_ = false;
  volatile uint32_t overflow_ = 0, underrun_ = 0;
  volatile int32_t gain_ = 32768;
  // yalnızca okuyan tarafın durumu
  bool playing_ = false, active_ = false;
  int fadePos_ = 0, rampLeft_ = 0;
  int16_t lastOut_ = 0;
  uint32_t lastDataMs_ = 0;
};
