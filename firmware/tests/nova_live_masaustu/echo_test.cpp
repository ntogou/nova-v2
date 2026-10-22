// Masaüstü testi: yankı silmenin donanımdan bağımsız parçaları (gecikme bulma, kapı, 24->16 kHz indirici)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "echo_math.h"
#include "resample.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  !! BASARISIZ (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rs = 12345;
static int16_t rnd(int amp) { rs = rs * 1664525u + 1013904223u; return (int16_t)((int32_t)(rs >> 16) % (2 * amp) - amp); }

int main() {
  printf("== 1) gecikme bulma: beyaz gürültü, yankı 0,3 kat, gecikme farklı değerlerde, ortam gürültüsüyle\n");
  const int DMAX = 4800, W = 16384, N = W + DMAX;
  int16_t *ref = new int16_t[N], *mic = new int16_t[W];
  const int delays[] = {17, 640, 1337, 1500, 4783};
  for (int t = 0; t < 5; t++) {
    int d = delays[t];
    for (int i = 0; i < N; i++) ref[i] = rnd(3000);
    // mic[k] = 0.3 * ref[k + DMAX - d] (+ yansıma) + gürültü
    for (int k = 0; k < W; k++) {
      float e = 0.3f * ref[k + DMAX - d] + (k + DMAX - d - 40 >= 0 ? 0.1f * ref[k + DMAX - d - 40] : 0);
      mic[k] = (int16_t)(e + rnd(300));
    }
    echomath::LagResult r = echomath::findLag(mic, ref, W, DMAX, [] {});
    CHECK(r.lag == d, "gecikme %d bulunamadi: %d (guven %.1f)", d, r.lag, r.quality);
    CHECK(r.quality > 8, "guven dusuk: %.1f", r.quality);
    printf("   gecikme %4d -> %4d, guven %.0f\n", d, r.lag, r.quality);
  }
  printf("== 2) yankı yoksa (yalnızca gürültü) güven düşük\n");
  for (int k = 0; k < W; k++) mic[k] = rnd(300);
  echomath::LagResult r0 = echomath::findLag(mic, ref, W, DMAX, [] {});
  CHECK(r0.quality < 8, "yankisiz kayitta guven yuksek: %.1f", r0.quality);
  printf("   guven %.1f\n", r0.quality);

  printf("== 3) kapı: Nova susarken açık; konuşurken artık yankıda kapalı, kullanıcı sesinde açık, sonra bir süre açık\n");
  echomath::EchoGate g;
  g.set(-30.0f, 8.0f, 8);                       // artık referansın 30 dB altı, pay 8 dB -> eşik ref * 10^(-22/20)
  CHECK(g.decide(10, 50), "Nova susarken kapi kapali");
  CHECK(!g.decide(5000, 5000 * 0.03f), "artik yankida kapi acik");       // -30 dB
  CHECK(!g.decide(5000, 5000 * 0.07f), "esigin altinda acik");           // -23 dB
  CHECK(g.decide(5000, 5000 * 0.2f), "kullanici sesinde kapi kapali");   // -14 dB
  int open = 0;
  for (int i = 0; i < 12; i++) open += g.decide(5000, 100) ? 1 : 0;
  CHECK(open == 8, "acik kalma %d parca (8 olmali)", open);
  CHECK(!g.decide(5000, 160), "taban esigi: Nova cok kisikken artik gecmemeli");

  printf("== 3b) kapı canlı orana uyar: ölçüm -30 dB, konuşmada artık -15 dB (2.8.0 kaydındaki gibi)\n");
  {
    echomath::EchoGate h;
    h.set(-30.0f, 8.0f, 8);
    unsigned sd = 12345;
    auto jit = [&]() { sd = sd * 1103515245u + 12345u; return ((sd >> 16) % 600) / 100.0f - 3.0f; };   // ±3 dB
    int acikIlk = 0, acikSon = 0;
    for (int i = 0; i < 300; i++) {                       // ~10 sn Nova konuşuyor, kullanıcı susuyor
      float ref = 3000 + 2000 * ((i * 7) % 5) / 4.0f;
      float out = ref * powf(10.0f, (-15.0f + jit()) / 20.0f);
      bool o = h.decide(ref, out);
      if (i < 30) acikIlk += o;
      if (i >= 200) acikSon += o;
    }
    printf("   ilk 1 sn açık %d/30, son 3 sn açık %d/100, canlı oran %.1f dB, eşik %.1f dB\n", acikIlk, acikSon, h.liveDb, h.thresholdDb());
    CHECK(acikSon <= 5, "Nova tek basina konusurken kapi hala acik: %d/100", acikSon);
    CHECK(h.liveDb > -19 && h.liveDb < -10, "canli oran %.1f", h.liveDb);
    // kullanıcı Nova'nın üstüne yüksek sesle konuşuyor: çıkış referansın 2 dB altı
    int acik = 0;
    for (int i = 0; i < 60; i++) acik += h.decide(4000, 4000 * 0.8f);   // ~2 sn araya girme
    CHECK(acik == 60, "kullanici sesinde kapi kapali kaldi: %d/60", acik);
    for (int i = 0; i < 9; i++) h.decide(4000, 4000 * 0.18f);   // açık kalma bitsin
    // hece bitti: referans bir anda sıfıra düştü ama odadaki yankı birkaç parça daha geliyor
    int kuyruk = 0;
    float yanki = 4000 * 0.18f;
    for (int i = 0; i < 6; i++, yanki *= 0.6f) kuyruk += h.decide(0, yanki);
    CHECK(kuyruk == 0, "yanki kuyrugu kapidan gecti: %d/6", kuyruk);
    // Nova tamamen sustu (zarf söndü): kapı açık
    for (int i = 0; i < 40; i++) h.decide(0, 20);
    CHECK(h.decide(0, 300), "Nova sustuktan sonra kapi kapali");
    // yeniden kurulumda (aynı ölçüm) canlı oran korunur, ölçüm değişince baştan
    float l = h.liveDb;
    h.set(-30.0f, 8.0f, 8);
    CHECK(h.liveDb == l, "ayni olcumle canli oran sifirlandi");
    h.set(-28.0f, 8.0f, 8);
    CHECK(h.liveDb == -28.0f, "yeni olcumde canli oran baştan baslamadi: %.1f", h.liveDb);
    // Tek parçalık zayıf patlamalı harf ("B", eşiğin +1,5 dB üstü) kapıyı açmaz; ardışık 2 parça açar (2.8.2)
    h.set(-26.0f, 8.0f, 8);                                // eşik -18 dB (ref * 0,126)
    CHECK(!h.decide(4000, 4000 * 0.145f), "tek parcalik patlamali harf kapiyi acti");
    CHECK(!h.decide(4000, 4000 * 0.05f), "sonraki sessiz parca acik");
    CHECK(!h.decide(4000, 4000 * 0.145f), "ikinci tekil parca ilk adimda acti");
    CHECK(h.decide(4000, 4000 * 0.155f), "ardisik ikinci parca kapiyi acmadi");
  }

  printf("== 3c) ses kısma adayı: Nova tek başına konuşurken nadiren, kullanıcı başlarken hemen (2.8.7; 2.8.8'de 4 dB / 3 parça)\n");
  {
    echomath::EchoGate h;
    h.set(-30.0f, 8.0f, 8);
    unsigned sd = 777;
    auto jit = [&]() { sd = sd * 1103515245u + 12345u; return ((sd >> 16) % 600) / 100.0f - 3.0f; };
    for (int i = 0; i < 300; i++) h.decide(4000, 4000 * powf(10.0f, (-15.0f + jit()) / 20.0f));   // öğrenme
    int yanlis = 0;
    for (int i = 0; i < 600; i++) {                      // 20 sn Nova tek başına
      h.decide(4000, 4000 * powf(10.0f, (-15.0f + jit()) / 20.0f));
      yanlis += h.candidate;
    }
    printf("   Nova tek başınayken aday parça %d/600\n", yanlis);
    CHECK(yanlis <= 6, "Nova kendi sesiyle sik kisma adayi: %d/600", yanlis);
    // kullanıcı eşiğin ~2,5 dB altında konuşmaya başlıyor (kapı açılmaz ama yakın): 3. parçada aday
    printf("   eşik %.1f dB, aday sınırı %.1f dB\n", h.thresholdDb(), h.thresholdDb() + 20 * log10f(h.duckFrac));
    float kul = h.thresholdDb() - 2.5f;
    int ilk = -1;
    for (int i = 0; i < 6; i++) {
      h.decide(4000, 4000 * powf(10.0f, kul / 20.0f));
      if (h.candidate && ilk < 0) ilk = i;
    }
    CHECK(ilk == 2, "kullanici baslarken aday %d. parcada (2 olmali)", ilk);
    // yankıdaki kısa patlama (2 parça, eşiğin 2 dB altı) aday değil
    for (int i = 0; i < 20; i++) h.decide(4000, 4000 * powf(10.0f, -15.0f / 20.0f));
    float pat = h.thresholdDb() - 2.0f;
    bool aday = false;
    for (int i = 0; i < 2; i++) { h.decide(4000, 4000 * powf(10.0f, pat / 20.0f)); aday |= h.candidate; }
    CHECK(!aday, "2 parcalik yanki patlamasi kismayi tetikledi");
    echomath::DuckGain d;
    float g1 = d.step(0.316f), g2 = d.step(0.316f);
    CHECK(g1 < 0.7f && g2 <= 0.32f, "kisma yavas: %.2f %.2f", g1, g2);
    int adim = 0;
    while (d.g < 0.999f && adim < 50) { d.step(1.0f); adim++; }
    CHECK(adim >= 8 && adim <= 12, "geri acilma %d blok (~10)", adim);
  }

  printf("== 3d) kullanıcı eşiğin biraz altında konuşunca eşik tırmanmaz, sonra normal sesle açılır (2.9.0)\n");
  {
    echomath::EchoGate h;
    h.set(-30.0f, 8.0f, 8);
    unsigned sd = 4242;
    auto jit = [&]() { sd = sd * 1103515245u + 12345u; return ((sd >> 16) % 600) / 100.0f - 3.0f; };
    for (int i = 0; i < 300; i++) h.decide(4000, 4000 * powf(10.0f, (-15.0f + jit()) / 20.0f));   // öğrenme
    float esik0 = h.thresholdDb();
    int acik = 0;
    for (int i = 0; i < 94; i++) acik += h.decide(4000, 4000 * powf(10.0f, (esik0 - 2.5f + jit() * 0.3f) / 20.0f));   // ~3 sn kısık söz
    float esik1 = h.thresholdDb();
    printf("   eşik önce %.1f dB, 3 sn kısık sözden sonra %.1f dB (açık %d/94)\n", esik0, esik1, acik);
    CHECK(esik1 - esik0 < 1.5f, "kisik sozde esik tirmandi: %.1f -> %.1f", esik0, esik1);
    int ilk = -1;
    for (int i = 0; i < 6; i++) if (h.decide(4000, 4000 * powf(10.0f, (esik0 + 3.0f) / 20.0f)) && ilk < 0) ilk = i;
    CHECK(ilk >= 0 && ilk <= 1, "normal seste kapi acilmadi (%d)", ilk);
    // Nova tek başına konuşurken kapı yine kapalı kalır
    for (int i = 0; i < 40; i++) h.decide(4000, 4000 * powf(10.0f, -15.0f / 20.0f));
    int yanlis = 0;
    for (int i = 0; i < 300; i++) yanlis += h.decide(4000, 4000 * powf(10.0f, (-15.0f + jit()) / 20.0f));
    CHECK(yanlis <= 6, "Nova tek basina konusurken kapi acik: %d/300", yanlis);
  }

  printf("== 4) 24 -> 16 kHz indirici: 1 kHz geçer, 10 kHz (16 kHz'te katlanırdı) süzülür, blok sınırında süreklilik\n");
  Downsampler24to16 ds;
  int16_t in[240], out[160];
  double p1 = 0, p10 = 0;
  long n1 = 0;
  float prevPh = 0;
  for (int b = 0; b < 100; b++) {
    for (int i = 0; i < 240; i++) in[i] = (int16_t)(10000 * sinf(2 * 3.14159265f * 1000.0f * (b * 240 + i) / 24000.0f));
    ds.process(in, 240, out);
    if (b > 2) for (int i = 0; i < 160; i++) { p1 += (double)out[i] * out[i]; n1++; }
  }
  float rms1 = sqrtf(p1 / n1);
  CHECK(fabsf(rms1 - 7071) < 150, "1 kHz seviyesi %.0f (7071 olmali)", rms1);
  Downsampler24to16 ds2;
  long n10 = 0;
  for (int b = 0; b < 100; b++) {
    for (int i = 0; i < 240; i++) in[i] = (int16_t)(10000 * sinf(2 * 3.14159265f * 10000.0f * (b * 240 + i) / 24000.0f));
    ds2.process(in, 240, out);
    if (b > 2) for (int i = 0; i < 160; i++) { p10 += (double)out[i] * out[i]; n10++; }
  }
  float db10 = 20 * log10f(sqrtf(p10 / n10) / 7071.0f);
  CHECK(db10 < -50, "10 kHz yeterince suzulmedi: %.1f dB", db10);
  printf("   1 kHz rms %.0f, 10 kHz %.1f dB\n", rms1, db10);
  // Süreklilik: tek seferde 480 ile 2 x 240 aynı sonucu vermeli
  Downsampler24to16 a, b2;
  int16_t big[480], o1[320], o2[320];
  for (int i = 0; i < 480; i++) big[i] = rnd(8000);
  a.process(big, 480, o1);
  b2.process(big, 240, o2);
  b2.process(big + 240, 240, o2 + 160);
  CHECK(!memcmp(o1, o2, sizeof(o1)), "blok bolununce sonuc degisti");
  (void)prevPh;

  printf("\nSONUC: %s (%d hata)\n", fails ? "BASARISIZ" : "GECTI", fails);
  return fails ? 1 : 0;
}
