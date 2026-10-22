#include "ekran.h"
#include "yazitipleri.h"
#include "config.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace ekran {

#define R565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return R565(r, g, b); }

namespace {

// A · Bakır Çekirdek paleti
const uint16_t ZEMIN = R565(0x0B, 0x07, 0x05);
const uint16_t BAKIR = R565(0xD4, 0x84, 0x4E);
const uint16_t KEHRIBAR = R565(0xF4, 0xB2, 0x6A);
const uint16_t SOLUK = R565(0x5A, 0x35, 0x20);
const uint16_t DERIN = R565(0x2A, 0x1A, 0x10);
const uint16_t KESIK = R565(0x3A, 0x25, 0x16);
const uint16_t KAPALI = R565(0x2E, 0x1D, 0x12);
const uint16_t YAZI = R565(0xF2, 0xE6, 0xD8);
const uint16_t SONUK = R565(0x9C, 0x85, 0x73);
const uint16_t SAATR = R565(0xC9, 0xAA, 0x8E);
const uint16_t KIRMIZI = R565(0xE8, 0x73, 0x5A);
const uint16_t ARKA = R565(0x1F, 0x13, 0x0B);

const float DERECE = 0.01745329f;
uint16_t *T = nullptr;

inline float sinir(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Tepeden saat yönünde açı (derece, 0..360). atan2f yerine polinom (hata ~0,001°): kartta her
// halka pikselinde çağrılıyor, kütüphanenin atan2f'i birkaç kat yavaş.
inline float aciDerece(float dx, float dy) {
  float y = dx, x = -dy;                       // atan2(dx, -dy)
  float ax = fabsf(x), ay = fabsf(y);
  float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (mx == 0) return 0;
  float z = mn / mx, z2 = z * z;
  float a = z * (0.9998660f + z2 * (-0.3302995f + z2 * (0.1801410f + z2 * (-0.0851330f + z2 * 0.0208351f))));
  if (ay > ax) a = 1.57079633f - a;
  if (x < 0) a = 3.14159265f - a;
  if (y < 0) a = -a;
  float d = a * 57.2957795f;
  return d < 0 ? d + 360.0f : d;
}

inline float sar360(float v) {                  // 0..360 aralığına (fmodf'tan hızlı)
  v -= 360.0f * floorf(v * (1.0f / 360.0f));
  return v >= 360.0f ? v - 360.0f : v;
}

inline uint16_t karis(uint16_t a, uint16_t b, int k) {      // k: 0 -> a, 256 -> b
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (((br - ar) * k) >> 8), g = ag + (((bg - ag) * k) >> 8), bl = ab + (((bb - ab) * k) >> 8);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

inline void boya(int x, int y, uint16_t c, int k) {
  if ((unsigned)x >= (unsigned)GEN || (unsigned)y >= (unsigned)YUK || k <= 0) return;
  uint16_t &p = T[y * GEN + x];
  p = k >= 256 ? c : karis(p, c, k);
}

void dolu(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > GEN) w = GEN - x;
  if (y + h > YUK) h = YUK - y;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) T[(y + j) * GEN + x + i] = c;
}

// Yumuşak kenarlı halka ya da yay. rd: dış yarıçap, ri: iç (0 = dolu daire). a0..a1: derece, tepeden saat yönünde.
void halka(float cx, float cy, float rd, float ri, uint16_t c, float a0 = 0, float a1 = 360) {
  bool tam = a1 - a0 >= 359.9f;
  int x0 = (int)floorf(cx - rd - 1), x1 = (int)ceilf(cx + rd + 1);
  int y0 = (int)floorf(cy - rd - 1), y1 = (int)ceilf(cy + rd + 1);
  float dis2 = (rd + 0.5f) * (rd + 0.5f), ic2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) : -1.0f;
  float acik = a1 - a0;
  for (int y = y0; y <= y1; y++) {
    if (y < 0 || y >= YUK) continue;
    for (int x = x0; x <= x1; x++) {
      if (x < 0 || x >= GEN) continue;
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
      if (d2 > dis2 || d2 < ic2) continue;
      float d = sqrtf(d2);
      float k = sinir(rd - d + 0.5f);
      if (ri > 0) k *= sinir(d - ri + 0.5f);
      if (!tam && k > 0) {
        float rel = sar360(aciDerece(dx, dy) - a0);
        if (rel > acik) {
          float uzak = fminf(rel - acik, 360.0f - rel) * d * DERECE;
          k *= sinir(0.5f - uzak);
        } else {
          k *= sinir(fminf(rel, acik - rel) * d * DERECE + 0.5f);
        }
      }
      boya(x, y, c, (int)(k * 256));
    }
  }
}

// Parçalı halka: n parça, her biri yay boyunca 'gen' piksel; 0. parça tepede (+donus derece). renk[i]
void parcali(float cx, float cy, float rd, float ri, int n, float gen, const uint16_t *renk, float donus) {
  float adim = 360.0f / n;
  int x0 = (int)floorf(cx - rd - 1), x1 = (int)ceilf(cx + rd + 1);
  int y0 = (int)floorf(cy - rd - 1), y1 = (int)ceilf(cy + rd + 1);
  float dis2 = (rd + 0.5f) * (rd + 0.5f), ic2 = (ri - 0.5f) * (ri - 0.5f);
  for (int y = y0; y <= y1; y++) {
    if (y < 0 || y >= YUK) continue;
    for (int x = x0; x <= x1; x++) {
      if (x < 0 || x >= GEN) continue;
      float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
      if (d2 > dis2 || d2 < ic2) continue;
      float d = sqrtf(d2);
      float k = sinir(rd - d + 0.5f) * sinir(d - ri + 0.5f);
      float aci = sar360(aciDerece(dx, dy) - donus + adim * 0.5f);
      int i = (int)(aci / adim);
      if (i >= n) i = n - 1;
      float sap = fabsf(aci - (i + 0.5f) * adim) * d * DERECE;
      k *= sinir(gen * 0.5f - sap + 0.5f);
      boya(x, y, renk[i], (int)(k * 256));
    }
  }
}

void kesikli(float cx, float cy, float r, int n, uint16_t c, float donus) {
  uint16_t renk[64];
  if (n > 64) n = 64;
  for (int i = 0; i < n; i++) renk[i] = c;
  parcali(cx, cy, r + 0.5f, r - 0.5f, n, 2.0f * 3.14159f * r / n * 0.55f, renk, donus);
}

void cubuk(int x, int y, int w, int h, uint16_t c) {
  if (h <= 0) return;
  dolu(x, y, w, h, c);
  if (w >= 3 && h >= 3) {                       // köşeler yumuşak
    boya(x, y, ZEMIN, 140); boya(x + w - 1, y, ZEMIN, 140);
    boya(x, y + h - 1, ZEMIN, 140); boya(x + w - 1, y + h - 1, ZEMIN, 140);
  }
}

// ---- yazı
const NFont *const FONT[4] = {&F_SAAT, &F_YAZI, &F_ETIKET, &F_KUCUK};

// UTF-8'den bir harf okur, yazı tipindeki koduna çevirir (Türkçe harfler 0x80 sonrasında)
int harfKodu(const char *&p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return (c == '\n' || c == '\t' || c == '\r') ? ' ' : c;
  uint32_t cp;
  int ek;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; ek = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; ek = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; ek = 3; }
  else return '?';
  for (int i = 0; i < ek; i++) {
    if (((uint8_t)*p & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | ((uint8_t)*p++ & 0x3F);
  }
  static const uint16_t OZEL[] = {0x11F, 0x11E, 0x131, 0x130, 0x15F, 0x15E, 0x2026, 0xB7, 0xB0, 0xE7,
                                  0xC7, 0xF6, 0xD6, 0xFC, 0xDC, 0xE2, 0xEE, 0xFB, 0x2013, 0x2019};
  for (int i = 0; i < (int)(sizeof(OZEL) / sizeof(OZEL[0])); i++)
    if (cp == OZEL[i]) return 0x80 + i;
  if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB) return '"';
  if (cp == 0x2018) return 0x80 + 19;
  if (cp == 0x2014) return 0x80 + 18;
  if (cp == 0xA0) return ' ';
  return '?';
}

const NGlif *glif(const NFont *f, int k) {
  if (k < f->ilk || k > f->son) {
    k = '?';
    if (k < f->ilk || k > f->son) return nullptr;
  }
  return &f->g[k - f->ilk];
}

// Harfi çizer; kalem 1/8 piksel biriminde (ilerlemeler kesirli, harf aralıkları düzgün kalsın)
int harf(const NFont *f, int kalem, int taban, int k, uint16_t c) {
  const NGlif *g = glif(f, k);
  if (!g) return kalem;
  int x = (kalem + 4) >> 3;
  const uint8_t *b = f->bmp + g->ofs;
  int i = 0;
  for (int yy = 0; yy < g->h; yy++)
    for (int xx = 0; xx < g->w; xx++, i++) {
      int a = (i & 1) ? (b[i >> 1] & 15) : (b[i >> 1] >> 4);
      if (a) boya(x + g->xo + xx, taban + g->yo + yy, c, a == 15 ? 256 : a * 17);
    }
  return kalem + g->adv8;
}

// bayt: en çok bu kadar bayt (-1 = hepsi). vurguBas: bu bayttan sonrası vurgu rengiyle.
int yaz(int font, int x, int taban, const char *s, uint16_t c, int aralik = 0, int bayt = -1, int vurguBas = -1,
        uint16_t vurgu = 0, int basOfs = 0) {
  const NFont *f = FONT[font];
  const char *bas = s;
  int kalem = x * 8;
  while (*s && (bayt < 0 || s - bas < bayt)) {
    int ofs = (int)(s - bas) + basOfs;
    int k = harfKodu(s);
    kalem = harf(f, kalem, taban, k, (vurguBas >= 0 && ofs >= vurguBas) ? vurgu : c) + aralik * 8;
  }
  return (kalem + 4) >> 3;
}

int genislik(int font, const char *s, int aralik, int bayt) {
  const NFont *f = FONT[font];
  const char *bas = s;
  int w = 0, n = 0;
  while (*s && (bayt < 0 || s - bas < bayt)) {
    const NGlif *g = glif(f, harfKodu(s));
    if (g) { w += g->adv8 + aralik * 8; n++; }
  }
  return n ? (w - aralik * 8 + 4) >> 3 : 0;
}

void ortala(int font, int taban, const char *s, uint16_t c, int aralik = 0) {
  yaz(font, (GEN - genislik(font, s, aralik, -1)) / 2, taban, s, c, aralik);
}

// Metni satırlara böler (kelime bölmeden). Son 'en' satırı bas[]/son[] (bayt) olarak döner.
int satirla(int font, const char *s, int maxW, int *bas, int *son, int en) {
  int n = 0;                         // toplam satır (yalnızca son 'en' tanesi tutulur)
  int satirBas = 0, i = 0, sonKelimeSonu = -1;
  int len = (int)strlen(s);
  while (satirBas < len && s[satirBas] == ' ') satirBas++;
  i = satirBas;
  while (i <= len) {
    bool bosluk = i == len || s[i] == ' ';
    if (bosluk) {
      int w = genislik(font, s + satirBas, 0, i - satirBas);
      if (w > maxW && sonKelimeSonu > satirBas) {
        int k = n % en;
        bas[k] = satirBas; son[k] = sonKelimeSonu; n++;
        satirBas = sonKelimeSonu;
        while (satirBas < len && s[satirBas] == ' ') satirBas++;
        sonKelimeSonu = -1;
        i = satirBas;
        continue;
      }
      sonKelimeSonu = i;
      if (i == len) break;
    }
    i++;
  }
  if (satirBas < len) {
    int k = n % en;
    bas[k] = satirBas; son[k] = len; n++;
  }
  // sırayı düzelt: en eski görünen satır başa
  int adet = n < en ? n : en;
  if (n > en) {
    int tb[8], ts[8];
    for (int j = 0; j < adet; j++) { tb[j] = bas[(n - adet + j) % en]; ts[j] = son[(n - adet + j) % en]; }
    for (int j = 0; j < adet; j++) { bas[j] = tb[j]; son[j] = ts[j]; }
  }
  return adet;
}

// Tek satıra sığmazsa sonunu "…" ile keser (bas = true: baştan gösterir; false: son kısmı gösterir)
void tekSatir(int font, int x, int taban, const char *s, int maxW, uint16_t c, bool bastan) {
  int len = (int)strlen(s);
  if (genislik(font, s, 0, -1) <= maxW) { yaz(font, x, taban, s, c); return; }
  int ucW = genislik(font, "…", 0, -1);
  if (bastan) {
    int b = len;
    while (b > 0 && genislik(font, s, 0, b) + ucW > maxW) {
      b--;
      while (b > 0 && ((uint8_t)s[b] & 0xC0) == 0x80) b--;
    }
    int xe = yaz(font, x, taban, s, c, 0, b);
    yaz(font, xe, taban, "…", c);
  } else {
    int a = 0;
    while (a < len && genislik(font, s + a, 0, -1) + ucW > maxW) {
      a++;
      while (a < len && ((uint8_t)s[a] & 0xC0) == 0x80) a++;
    }
    int xe = yaz(font, x, taban, "…", c);
    yaz(font, xe, taban, s + a, c);
  }
}

// ---- parçalar
void pilSimge(int x, int y, int yuzde, uint16_t c) {   // 12x6
  dolu(x, y, 10, 1, c); dolu(x, y + 5, 10, 1, c);
  dolu(x, y, 1, 6, c); dolu(x + 9, y, 1, 6, c);
  dolu(x + 10, y + 2, 1, 2, c);
  if (yuzde > 0) dolu(x + 2, y + 2, (6 * (yuzde > 100 ? 100 : yuzde) + 50) / 100, 2, yuzde <= 15 ? KIRMIZI : c);
}

void wifiSimge(int x, int y, bool var, int rssi, uint16_t c) {   // 4 çubuk, 11x6
  int n = !var ? 0 : (rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1);
  for (int i = 0; i < 4; i++) {
    int h = 2 + i + (i == 3);
    dolu(x + i * 3, y + 6 - h, 2, h, i < n ? c : KAPALI);
  }
}

void ustSerit(const Veri &v) {
  yaz(3, 6, 11, "NOVA", BAKIR, 2);
  wifiSimge(127, 5, v.wifi, v.rssi, BAKIR);
  pilSimge(142, 5, v.pil, SONUK);
}

void cekirdek(float cx, float cy, float r, uint16_t renk, uint16_t nokta) {
  halka(cx, cy, r + 4.0f, r + 3.0f, SOLUK);
  halka(cx, cy, r + 3.0f, r, DERIN);
  halka(cx, cy, r, 0, renk);
  halka(cx, cy, r * 0.4f, 0, nokta);
}

const char *const GUN_TR[7] = {"PAZ", "PZT", "SAL", "ÇAR", "PER", "CUM", "CMT"};
const char *const AY_TR[12] = {"OCA", "ŞUB", "MAR", "NİS", "MAY", "HAZ", "TEM", "AĞU", "EYL", "EKİ", "KAS", "ARA"};
const char *const GUN_EN[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const char *const AY_EN[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
const char *const *const GUN = UI_TR(GUN_TR, GUN_EN);
const char *const *const AY = UI_TR(AY_TR, AY_EN);
// Yüzde: Türkçede işaret önde (%79), İngilizcede arkada (79%)
const char *const YUZDE = UI_TR("%%%d", "%d%%");

// ---- durumlar
void uyku(const Veri &v) {
  kesikli(80, 60, 54, 40, KAPALI, v.t * 2.0f);
  halka(80, 60, 46.5f, 45.5f, SOLUK, 245 - v.t * 4.0f, 245 - v.t * 4.0f + 180);
  char s[16];
  if (v.saat >= 0) snprintf(s, sizeof(s), "%02d:%02d", v.saat, v.dakika);
  else snprintf(s, sizeof(s), "--:--");
  ortala(0, 70, s, SAATR, -1);
  if (v.saat >= 0 && v.ay >= 1 && v.ay <= 12) {
    char t[40];
    snprintf(t, sizeof(t), "%s · %d %s", GUN[v.haftaGunu % 7], v.gun, AY[v.ay - 1]);
    ortala(2, 87, t, BAKIR, 2);
  }
  if (v.bildirim && *v.bildirim) {
    dolu(0, 104, GEN, 24, ZEMIN);
    int w = genislik(3, v.bildirim, 0, -1);
    if (w <= 148) yaz(3, (GEN - w) / 2, 120, v.bildirim, KEHRIBAR);
    else tekSatir(3, 6, 120, v.bildirim, 148, KEHRIBAR, true);
    return;
  }
  if (v.bootIpucu) yaz(3, 7, 121, UI_TR("BOOT'a bas", "press BOOT"), SONUK);
  else if (v.hava && *v.hava) tekSatir(3, 7, 121, v.hava, 96, SONUK, true);
  char p[16];
  if (v.pil >= 0) {
    snprintf(p, sizeof(p), YUZDE, v.pil);
    int w = genislik(3, p, 0, -1);
    yaz(3, 137 - w, 121, p, SONUK);
  }
  pilSimge(141, 115, v.pil, SONUK);
}

void dinliyor(const Veri &v) {
  ustSerit(v);
  kesikli(80, 62, 42, 36, KESIK, v.t * 4.0f);
  uint16_t renk[24];
  int yanan = (int)(v.mik * 24 + 0.5f);
  for (int i = 0; i < 24; i++) renk[i] = i < yanan ? KEHRIBAR : KAPALI;
  parcali(80, 62, 36.5f, 27.5f, 24, 3.0f, renk, 0);
  float nefes = 0.5f + 0.5f * sinf(v.t * 3.9f);
  cekirdek(80, 62, 10, karis(BAKIR, KEHRIBAR, (int)(nefes * 110)), KEHRIBAR);
  if (v.bildirim && *v.bildirim) {
    if (genislik(2, v.bildirim, 0, -1) <= 148) ortala(2, 119, v.bildirim, KEHRIBAR);
    else tekSatir(2, 6, 119, v.bildirim, 148, KEHRIBAR, true);
  } else if (v.kullanici && *v.kullanici) {
    tekSatir(2, 6, 119, v.kullanici, 148, SONUK, false);
  } else {
    ortala(2, 117, UI_TR("dinliyorum", "listening"), KEHRIBAR, 1);
  }
}

void dusunuyor(const Veri &v) {
  ustSerit(v);
  float a = v.t * 150.0f;
  halka(80, 56, 36, 34, ARKA);
  halka(80, 56, 36, 34, BAKIR, a, a + 160);
  halka(80, 56, 25, 24, ARKA);
  halka(80, 56, 25, 24, KEHRIBAR, -v.t * 100.0f, -v.t * 100.0f + 90);
  halka(80, 56, 6, 0, SOLUK);
  const char *et = (v.etkinlik && *v.etkinlik) ? v.etkinlik : UI_TR("düşünüyorum…", "thinking…");
  ortala(1, 106, et, YAZI);
  if (v.etkinlikSn >= 0) {
    char s[32];
    snprintf(s, sizeof(s), UI_TR("arka model · %d sn", "backend · %d s"), v.etkinlikSn);
    ortala(3, 120, s, SONUK);
  }
}

void konusuyor(const Veri &v) {
  kesikli(28, 28, 18, 24, KESIK, v.t * 6.0f);
  uint16_t renk[16];
  int yanan = (int)(v.ses * 16 + 0.5f);
  for (int i = 0; i < 16; i++) renk[i] = i < yanan ? BAKIR : KAPALI;
  parcali(28, 28, 15.5f, 10.5f, 16, 2.0f, renk, 0);
  cekirdek(28, 28, 4, KEHRIBAR, R565(0xFF, 0xE3, 0xBC));
  for (int i = 0; i < 14; i++) {
    float sekil = 1.0f - fabsf(i - 6.5f) / 9.0f;
    float oyna = 0.65f + 0.35f * sinf(v.t * (6.0f + i * 0.9f) + i * 1.7f);
    int h = 2 + (int)(v.ses * sekil * oyna * 30.0f);
    cubuk(56 + i * 6, 28 - h / 2, 4, h, h > 20 ? KEHRIBAR : BAKIR);
  }
  int ust = 62;
  if (v.kullanici && *v.kullanici) {
    char b[200];
    snprintf(b, sizeof(b), UI_TR("sen: %s", "you: %s"), v.kullanici);
    tekSatir(2, 6, 58, b, 148, SONUK, true);
    ust = 64;
  }
  if (v.altyazi && *v.altyazi) {
    int bas[4], son[4];
    int n = satirla(1, v.altyazi, 148, bas, son, 4);
    for (int i = 0; i < n; i++)
      yaz(1, 6, ust + 14 + i * 15, v.altyazi + bas[i], YAZI, 0, son[i] - bas[i], v.vurguBas, KEHRIBAR, bas[i]);
  }
}

void acilis(const Veri &v, const char *yazi) {
  yaz(3, 6, 11, "NOVA", BAKIR, 2);
  uint16_t renk[24];
  int bas = (int)(v.t * 14.0f) % 24;
  for (int i = 0; i < 24; i++) {
    int geri = (bas - i + 24) % 24;
    renk[i] = geri == 0 ? KEHRIBAR : geri < 3 ? BAKIR : geri < 6 ? SOLUK : KAPALI;
  }
  parcali(80, 58, 30.5f, 23.5f, 24, 3.0f, renk, 0);
  cekirdek(80, 58, 7, SOLUK, BAKIR);
  if (v.bildirim && *v.bildirim) {           // bakım kipi adresi, kurulum hatası gibi notlar: etiketin yerine
    int bas[3], son[3];
    int n = satirla(3, v.bildirim, 150, bas, son, 3);
    int taban = n == 1 ? 112 : (n == 2 ? 106 : 100);
    for (int i = 0; i < n; i++) {
      int w = genislik(3, v.bildirim + bas[i], 0, son[i] - bas[i]);
      yaz(3, (GEN - w) / 2, taban + i * 12, v.bildirim + bas[i], KEHRIBAR, 0, son[i] - bas[i]);
    }
  } else {
    ortala(2, 112, yazi, SONUK, 1);
  }
}

void guncelleme(const Veri &v) {
  yaz(3, 6, 11, "NOVA", BAKIR, 2);
  uint16_t renk[24];
  int yanan = v.ota * 24 / 100;
  for (int i = 0; i < 24; i++) renk[i] = i < yanan ? KEHRIBAR : KAPALI;
  parcali(80, 58, 36.5f, 27.5f, 24, 3.0f, renk, 0);
  char s[12];
  snprintf(s, sizeof(s), YUZDE, v.ota);
  ortala(1, 63, s, YAZI);
  ortala(2, 112, UI_TR("güncelleniyor", "updating"), SONUK, 1);
}

void pilBitti(const Veri &) {
  dolu(52, 44, 52, 2, KIRMIZI); dolu(52, 72, 52, 2, KIRMIZI);
  dolu(52, 44, 2, 30, KIRMIZI); dolu(102, 44, 2, 30, KIRMIZI);
  dolu(104, 53, 4, 12, KIRMIZI);
  dolu(56, 48, 8, 22, KIRMIZI);
  ortala(2, 104, UI_TR("pil bitti", "battery empty"), KIRMIZI, 1);
  ortala(3, 118, UI_TR("kapanıyorum", "shutting down"), SONUK);
}

}  // namespace

int yaziGenislik(int font, const char *s, int aralik, int bayt) { return genislik(font, s, aralik, bayt); }

size_t temizle(const char *src, char *out, size_t cap, bool bosBasla) {
  size_t n = 0;
  if (!cap) return 0;
  bool bosluk = bosBasla;
  const uint8_t *p = (const uint8_t *)(src ? src : "");
  while (*p && n + 5 < cap) {
    uint8_t c = *p;
    if (c < 0x80) {
      p++;
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c < 0x20 || c == 0x7F) continue;
      if (c == ' ') {
        if (!bosluk) out[n++] = ' ';
        bosluk = true;
      } else {
        out[n++] = (char)c;
        bosluk = false;
      }
      continue;
    }
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (!len) { p++; continue; }                 // yarım ya da bozuk bayt
    int i = 1;
    while (i < len && (p[i] & 0xC0) == 0x80) i++;
    if (i < len) { p += i; continue; }
    uint32_t cp = c & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
    for (i = 1; i < len; i++) cp = (cp << 6) | (p[i] & 0x3F);
    if (cp == 0xA0) {                             // bölünmez boşluk
      if (!bosluk) out[n++] = ' ';
      bosluk = true;
    } else if (len <= 3 && cp >= 0xA0 && cp < 0x2100) {
      memcpy(out + n, p, len);
      n += len;
      bosluk = false;
    }
    p += len;
  }
  out[n] = 0;
  return n;
}

void ekle(char *buf, size_t maxLen, const char *src, int *vurgu, char *gecici, size_t gcap) {
  size_t len = strlen(buf);
  size_t n = temizle(src, gecici, gcap, len == 0 || buf[len - 1] == ' ');
  if (n > maxLen) {                               // tek parça bile sığmıyor: sonunu tut
    size_t a = n - maxLen / 2;
    while (a < n && ((uint8_t)gecici[a] & 0xC0) == 0x80) a++;
    memmove(gecici, gecici + a, n - a + 1);
    n -= a;
    len = 0;
    buf[0] = 0;
  }
  if (len + n > maxLen) {
    size_t kes = len + n - maxLen + maxLen / 4;   // biraz fazlası: her eklemede kırpılmasın
    if (kes > len) kes = len;
    while (kes < len && buf[kes] != ' ') kes++;
    while (kes < len && buf[kes] == ' ') kes++;
    memmove(buf, buf + kes, len - kes + 1);
    len -= kes;
  }
  if (vurgu) *vurgu = (int)len;
  memcpy(buf + len, gecici, n + 1);
}

void ciz(uint16_t *tampon, const Veri &v) {
  T = tampon;
  for (int i = 0; i < GEN * YUK; i++) T[i] = ZEMIN;
  switch (v.durum) {
    case UYKU: uyku(v); break;
    case DINLIYOR: dinliyor(v); break;
    case DUSUNUYOR: dusunuyor(v); break;
    case KONUSUYOR: konusuyor(v); break;
    case ACILIS: acilis(v, UI_TR("açılıyor", "starting")); break;
    case WIFI: acilis(v, UI_TR("WiFi aranıyor", "finding WiFi")); break;
    case BAGLANIYOR: acilis(v, UI_TR("bağlanıyor", "connecting")); break;
    case GUNCELLEME: guncelleme(v); break;
    case PIL_BITTI: pilBitti(v); break;
  }
}

}  // namespace ekran
