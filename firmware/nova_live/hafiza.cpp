#include "hafiza.h"
#include "config.h"
#include "psbuf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef NOVA_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_rom_crc.h"
#endif

namespace hafiza {

namespace {

struct Not { uint16_t id; int32_t ymd; char kaynak; char metin[HAFIZA_METIN]; };
struct Zam { uint16_t id; int64_t vakit; char tur; char etiket[64]; };

Not *notlar = nullptr;           // HAFIZA_MAX_NOT kayıt, PSRAM'de
int notN = 0;
Zam *zam = nullptr;              // ZAMANLAYICI_MAX kayıt, PSRAM'de
int zamN = 0;
// Çalışma tamponları (iç RAM şifreli bağlantıya kalsın diye PSRAM'de; hepsi kilit altında kullanılır)
struct Tampon {
  char temiz[HAFIZA_METIN];
  char katli[HAFIZA_METIN * 2];
  char diger[HAFIZA_METIN * 2];
  char sorgu[HAFIZA_METIN * 2];
  char icerik[HAFIZA_METIN * 2];
  char satir[HAFIZA_METIN + 96];
};
Tampon *T = nullptr;
uint16_t sonId = 0, sonZamId = 0;
bool kirli = false;
Depo depo;
int32_t tz = 10800;
char *kayitBuf = nullptr;        // KAYIT_CAP bayt, PSRAM'de
const size_t KAYIT_CAP = 24 * 1024;

#ifndef NOVA_HOST_TEST
SemaphoreHandle_t kilit = nullptr;
struct Kilit {
  Kilit() { if (kilit) xSemaphoreTakeRecursive(kilit, portMAX_DELAY); }
  ~Kilit() { if (kilit) xSemaphoreGiveRecursive(kilit); }
};
#else
struct Kilit { Kilit() {} ~Kilit() {} };
#endif

// UTF-8 harfini okuyup yalın karşılığını verir; n: harfin bayt sayısı
char yalin(const unsigned char *p, int *n) {
  unsigned c = p[0];
  if (c < 0x80) {
    *n = 1;
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return (char)c;
    return ' ';
  }
  if ((c & 0xE0) == 0xC0 && p[1]) {
    *n = 2;
    unsigned u = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    switch (u) {
      case 0xC7: case 0xE7: return 'c';
      case 0xD6: case 0xF6: return 'o';
      case 0xDC: case 0xFC: case 0xDB: case 0xFB: return 'u';
      case 0xC2: case 0xE2: return 'a';
      case 0xCE: case 0xEE: case 0x130: case 0x131: return 'i';
      case 0x11E: case 0x11F: return 'g';
      case 0x15E: case 0x15F: return 's';
    }
    return ' ';
  }
  *n = (c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 1);
  for (int i = 1; i < *n; i++) if (!p[i]) { *n = i; break; }
  return ' ';
}

// Metni tek satıra indirir, baştaki/sondaki boşluğu atar; sığmazsa UTF-8 harfini ortadan bölmeden keser
void temizKopya(char *dst, size_t cap, const char *src) {
  while (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r') src++;
  size_t o = 0;
  const char *s = src;
  while (*s && o + 1 < cap) {
    char ch = *s++;
    if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    dst[o++] = ch;
  }
  if (*s && ((uint8_t)*s & 0xC0) == 0x80) {        // bir harfin ortasında kesildi: o harfi tümden at
    while (o > 0 && ((uint8_t)dst[o - 1] & 0xC0) == 0x80) o--;
    if (o > 0) o--;
  }
  while (o > 0 && dst[o - 1] == ' ') o--;
  dst[o] = 0;
}

bool icerir(const char *metin, const char *sorguKatli) {
  katla(metin, T->icerik, sizeof(T->icerik));
  return *sorguKatli && strstr(T->icerik, sorguKatli) != nullptr;
}

// "#12" -> 12, yoksa -1
int numara(const char *s) {
  while (*s == ' ') s++;
  if (*s != '#') return -1;
  int v = atoi(s + 1);
  return v > 0 ? v : -1;
}

bool hepsiMi(const char *katli) {
  return !strcmp(katli, "hepsi") || !strcmp(katli, "hepsini") || !strcmp(katli, "tumu") || !strcmp(katli, "tumunu");
}

// Gün sayısından takvime (Howard Hinnant'ın algoritması)
void takvim(int64_t gunler, int *y, int *m, int *d) {
  gunler += 719468;
  int64_t era = (gunler >= 0 ? gunler : gunler - 146096) / 146097;
  unsigned doe = (unsigned)(gunler - era * 146097);
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t yy = (int64_t)yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned dd = doy - (153 * mp + 2) / 5 + 1;
  unsigned mm = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (mm <= 2));
  *m = (int)mm;
  *d = (int)dd;
}
int64_t gunSayisi(int y, int m, int d) {
  y -= m <= 2;
  int64_t era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void yukle(const char *buf, size_t len) {
  notN = 0;
  zamN = 0;
  const char *p = buf, *end = buf + len;
  char *satir = T->satir;
  const size_t satirCap = sizeof(T->satir);
  while (p < end) {
    const char *e = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (!e) e = end;
    size_t n = (size_t)(e - p);
    if (n >= satirCap) n = satirCap - 1;
    memcpy(satir, p, n);
    satir[n] = 0;
    p = e + 1;
    if (satir[0] == 'N' && satir[1] == ' ' && notN < HAFIZA_MAX_NOT) {
      unsigned id = 0; long ymd = 0; char k = 0; int off = 0;
      if (sscanf(satir + 2, "%u %ld %c %n", &id, &ymd, &k, &off) >= 3 && off > 0) {
        Not &x = notlar[notN++];
        x.id = (uint16_t)id;
        x.ymd = (int32_t)ymd;
        x.kaynak = k;
        temizKopya(x.metin, sizeof(x.metin), satir + 2 + off);
        if (id > sonId) sonId = (uint16_t)id;
      }
    } else if (satir[0] == 'Z' && satir[1] == ' ' && zamN < ZAMANLAYICI_MAX) {
      unsigned id = 0; long long v = 0; char t = 0; int off = 0;
      if (sscanf(satir + 2, "%u %lld %c %n", &id, &v, &t, &off) >= 3 && off > 0) {
        Zam &z = zam[zamN++];
        z.id = (uint16_t)id;
        z.vakit = (int64_t)v;
        z.tur = t;
        temizKopya(z.etiket, sizeof(z.etiket), satir + 2 + off);
        if (id > sonZamId) sonZamId = (uint16_t)id;
      }
    }
  }
}

size_t yaziyaDok(char *out, size_t cap) {
  size_t o = 0;
  int n = snprintf(out, cap, "NOVA-HAFIZA 1\n");
  if (n < 0 || (size_t)n >= cap) return 0;
  o = (size_t)n;
  for (int i = 0; i < notN; i++) {
    n = snprintf(out + o, cap - o, "N %u %ld %c %s\n", notlar[i].id, (long)notlar[i].ymd, notlar[i].kaynak, notlar[i].metin);
    if (n < 0 || (size_t)n >= cap - o) return 0;
    o += (size_t)n;
  }
  for (int i = 0; i < zamN; i++) {
    n = snprintf(out + o, cap - o, "Z %u %lld %c %s\n", zam[i].id, (long long)zam[i].vakit, zam[i].tur, zam[i].etiket);
    if (n < 0 || (size_t)n >= cap - o) return 0;
    o += (size_t)n;
  }
  return o;
}

const char *kaynakAdi(char k) {
  return k == KAYNAK_KULLANICI ? "senin isteğin" : k == KAYNAK_ONAYLI ? "onayladın" : "Nova";
}

}  // namespace

void katla(const char *in, char *out, size_t cap) {
  size_t o = 0;
  bool bosluk = true;
  const unsigned char *p = (const unsigned char *)in;
  while (*p && o + 1 < cap) {
    int n = 1;
    char c = yalin(p, &n);
    p += n;
    if (c == ' ') { if (!bosluk) { out[o++] = ' '; bosluk = true; } continue; }
    out[o++] = c;
    bosluk = false;
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
}

int64_t yerelEpoch(int yil, int ay, int gun, int saat, int dakika) {
  return gunSayisi(yil, ay, gun) * 86400 + saat * 3600 + dakika * 60 - tz;
}

void yerelTakvim(int64_t vakit, int *yil, int *ay, int *gun, int *saat, int *dakika, int *haftaGunu) {
  int64_t l = vakit + tz;
  int64_t g = l >= 0 ? l / 86400 : (l - 86399) / 86400;
  int64_t s = l - g * 86400;
  int y, m, d;
  takvim(g, &y, &m, &d);
  if (yil) *yil = y;
  if (ay) *ay = m;
  if (gun) *gun = d;
  if (saat) *saat = (int)(s / 3600);
  if (dakika) *dakika = (int)(s % 3600 / 60);
  if (haftaGunu) *haftaGunu = (int)(((g % 7) + 11) % 7);     // 0 = Pazar (1970-01-01 Perşembe)
}

void saatYaz(int64_t vakit, int64_t simdi, char *out, size_t cap) {
  int y, m, d, h, mi, y2, m2, d2;
  yerelTakvim(vakit, &y, &m, &d, &h, &mi, nullptr);
  yerelTakvim(simdi, &y2, &m2, &d2, nullptr, nullptr, nullptr);
  int64_t fark = gunSayisi(y, m, d) - gunSayisi(y2, m2, d2);
  if (fark == 0) snprintf(out, cap, "%02d:%02d", h, mi);
  else if (fark == 1) snprintf(out, cap, "yarın %02d:%02d", h, mi);
  else snprintf(out, cap, "%02d.%02d %02d:%02d", d, m, h, mi);
}

bool begin(const Depo &d, int32_t tzSaniye) {
#ifndef NOVA_HOST_TEST
  if (!kilit) kilit = xSemaphoreCreateRecursiveMutex();
#endif
  Kilit k;
  depo = d;
  tz = tzSaniye;
  if (!psLazy(notlar, HAFIZA_MAX_NOT) || !psLazy(zam, ZAMANLAYICI_MAX) || !psLazy(T, 1) || !psLazy(kayitBuf, KAYIT_CAP)) return false;
  notN = zamN = 0;
  sonId = sonZamId = 0;
  kirli = false;
  size_t len = 0;
  if (depo.oku && depo.oku(kayitBuf, KAYIT_CAP - 1, &len) && len > 14 && !memcmp(kayitBuf, "NOVA-HAFIZA 1\n", 14)) {
    kayitBuf[len] = 0;
    yukle(kayitBuf + 14, len - 14);
    if (depo.tasindi && depo.tasindi()) kirli = true;   // eski yerden geldi: ilk boş anda yeni yere yazılır
  }
  return true;
}

int ekle(const char *metin, char kaynak, int32_t ymd, char *msg, size_t cap) {
  Kilit k;
  if (!notlar || !T) { snprintf(msg, cap, "hafıza kurulamadı"); return -1; }
  char *temiz = T->temiz, *katli = T->katli, *diger = T->diger;
  temizKopya(temiz, sizeof(T->temiz), metin ? metin : "");
  katla(temiz, katli, sizeof(T->katli));
  if (strlen(katli) < 3) { snprintf(msg, cap, "kaydedilecek bir şey yok"); return -1; }
  for (int i = 0; i < notN; i++) {
    katla(notlar[i].metin, diger, sizeof(T->diger));
    if (!strcmp(diger, katli)) { snprintf(msg, cap, "zaten kayıtlı (#%u)", notlar[i].id); return notlar[i].id; }
  }
  if (notN >= HAFIZA_MAX_NOT) {
    snprintf(msg, cap, "hafıza dolu (%d not); önce bir şey silinmeli", HAFIZA_MAX_NOT);
    return -1;
  }
  if (kaynak != KAYNAK_KULLANICI && kaynak != KAYNAK_ONAYLI) kaynak = KAYNAK_NOVA;
  Not &x = notlar[notN++];
  x.id = ++sonId;
  x.ymd = ymd;
  x.kaynak = kaynak;
  memcpy(x.metin, temiz, sizeof(x.metin));
  kirli = true;
  snprintf(msg, cap, "kaydedildi (#%u): %s", x.id, x.metin);
  return x.id;
}

int unut(const char *sorgu, char *msg, size_t cap) {
  Kilit k;
  if (!T) { snprintf(msg, cap, "hafıza kurulamadı"); return 0; }
  char *q = T->sorgu;
  katla(sorgu ? sorgu : "", q, sizeof(T->sorgu));
  int no = numara(sorgu ? sorgu : "");
  bool hepsi = hepsiMi(q);
  int eslesen = 0;
  for (int i = 0; i < notN; i++)
    if (hepsi || (no > 0 ? notlar[i].id == no : icerir(notlar[i].metin, q))) eslesen++;
  if (!eslesen) { snprintf(msg, cap, "eşleşen not yok"); return 0; }
  if (!hepsi && no < 0 && eslesen > 3) {
    snprintf(msg, cap, "%d not eşleşti; hangisi olduğunu daha belirgin söyle", eslesen);
    return 0;
  }
  size_t o = 0;
  int n = snprintf(msg, cap, "silindi: ");
  o = n > 0 && (size_t)n < cap ? (size_t)n : 0;
  int sil = 0;
  for (int i = 0; i < notN;) {
    if (hepsi || (no > 0 ? notlar[i].id == no : icerir(notlar[i].metin, q))) {
      if (!hepsi && o < cap) {
        n = snprintf(msg + o, cap - o, "%s%s", sil ? "; " : "", notlar[i].metin);
        if (n > 0) o = (size_t)n < cap - o ? o + (size_t)n : cap - 1;
      }
      memmove(&notlar[i], &notlar[i + 1], (size_t)(notN - i - 1) * sizeof(Not));
      notN--;
      sil++;
      continue;
    }
    i++;
  }
  if (hepsi) snprintf(msg, cap, "bütün notlar silindi (%d)", sil);
  kirli = true;
  return sil;
}

int notSayisi() { return notN; }

size_t promptaYaz(char *out, size_t cap) {
  Kilit k;
  if (!notN || cap < 64) { if (cap) out[0] = 0; return 0; }
  size_t o = 0;
  int n = snprintf(out, cap, "Hafızan (kullanıcı hakkında kalıcı notların, eskiden yeniye; tarih gün.ay):\n");
  if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
  o = (size_t)n;
  for (int i = 0; i < notN; i++) {
    int32_t ymd = notlar[i].ymd;
    if (ymd > 0) n = snprintf(out + o, cap - o, "- (%02d.%02d) %s\n", (int)(ymd % 100), (int)(ymd / 100 % 100), notlar[i].metin);
    else n = snprintf(out + o, cap - o, "- %s\n", notlar[i].metin);
    if (n < 0 || (size_t)n >= cap - o) break;        // sığmayan son notlar bu oturumda verilmez
    o += (size_t)n;
  }
  return o;
}

size_t listele(char *out, size_t cap) {
  Kilit k;
  size_t o = 0;
  int n = snprintf(out, cap, "[hafıza] %d not (sınır %d)%s\n", notN, HAFIZA_MAX_NOT, notN ? ". Silmek için sayfada: /unut 3 ya da /unut hepsi" : "");
  if (n < 0 || (size_t)n >= cap) return 0;
  o = (size_t)n;
  for (int i = 0; i < notN; i++) {
    int32_t ymd = notlar[i].ymd;
    char tarih[12] = "--.--";
    if (ymd > 0) snprintf(tarih, sizeof(tarih), "%02d.%02d", (int)(ymd % 100), (int)(ymd / 100 % 100));
    n = snprintf(out + o, cap - o, "[hafıza]   #%u (%s, %s) %s\n", notlar[i].id, tarih, kaynakAdi(notlar[i].kaynak), notlar[i].metin);
    if (n < 0 || (size_t)n >= cap - o) break;
    o += (size_t)n;
  }
  return o;
}

size_t numaraliListe(char *out, size_t cap) {
  Kilit k;
  if (!cap) return 0;
  out[0] = 0;
  size_t o = 0;
  for (int i = 0; i < notN; i++) {
    int n = snprintf(out + o, cap - o, "#%u %s\n", notlar[i].id, notlar[i].metin);
    if (n < 0 || (size_t)n >= cap - o) break;
    o += (size_t)n;
  }
  return o;
}

void notlariGez(void (*f)(void *ctx, int id, int32_t ymd, char kaynak, const char *metin), void *ctx) {
  Kilit k;
  for (int i = 0; i < notN; i++) f(ctx, notlar[i].id, notlar[i].ymd, notlar[i].kaynak, notlar[i].metin);
}

int kur(int64_t vakit, char tur, const char *etiket, int64_t simdi, char *msg, size_t cap) {
  Kilit k;
  if (!zam) { snprintf(msg, cap, "hafıza kurulamadı"); return -1; }
  if (vakit <= simdi) { snprintf(msg, cap, "bu zaman geçmişte kalıyor"); return -1; }
  if (vakit - simdi > 30LL * 86400) { snprintf(msg, cap, "en çok 30 gün sonrası kurulabilir"); return -1; }
  if (zamN >= ZAMANLAYICI_MAX) { snprintf(msg, cap, "en çok %d zamanlayıcı kurulabilir; önce birini iptal et", ZAMANLAYICI_MAX); return -1; }
  Zam &z = zam[zamN++];
  z.id = ++sonZamId;
  z.vakit = vakit;
  z.tur = tur == ALARM ? ALARM : ZAMANLAYICI;
  temizKopya(z.etiket, sizeof(z.etiket), etiket && *etiket ? etiket : (tur == ALARM ? "alarm" : "zamanlayıcı"));
  kirli = true;
  char s[24];
  saatYaz(vakit, simdi, s, sizeof(s));
  int64_t dk = (vakit - simdi + 30) / 60;
  if (dk < 120) snprintf(msg, cap, "kuruldu (#%u): %s, %s (%lld dk sonra)", z.id, z.etiket, s, (long long)dk);
  else snprintf(msg, cap, "kuruldu (#%u): %s, %s", z.id, z.etiket, s);
  return z.id;
}

int iptal(const char *sorgu, char *msg, size_t cap) {
  Kilit k;
  if (!T) { snprintf(msg, cap, "hafıza kurulamadı"); return 0; }
  char *q = T->sorgu;
  katla(sorgu ? sorgu : "", q, sizeof(T->sorgu));
  int no = numara(sorgu ? sorgu : "");
  bool hepsi = hepsiMi(q) || !q[0];
  if (!q[0] && zamN > 1) { snprintf(msg, cap, "hangisi? %d zamanlayıcı var", zamN); return 0; }
  int sil = 0;
  size_t o = 0;
  int n = snprintf(msg, cap, "iptal edildi: ");
  o = n > 0 && (size_t)n < cap ? (size_t)n : 0;
  for (int i = 0; i < zamN;) {
    if (hepsi || (no > 0 ? zam[i].id == no : icerir(zam[i].etiket, q))) {
      n = snprintf(msg + o, cap - o, "%s%s", sil ? ", " : "", zam[i].etiket);
      if (n > 0) o = (size_t)n < cap - o ? o + (size_t)n : cap - 1;
      memmove(&zam[i], &zam[i + 1], (size_t)(zamN - i - 1) * sizeof(Zam));
      zamN--;
      sil++;
      continue;
    }
    i++;
  }
  if (!sil) snprintf(msg, cap, "eşleşen zamanlayıcı yok");
  else kirli = true;
  return sil;
}

size_t zamanlayicilar(char *out, size_t cap, int64_t simdi) {
  Kilit k;
  if (!cap) return 0;
  out[0] = 0;
  size_t o = 0;
  // Vakte göre sırala (en yakın önce)
  for (int i = 1; i < zamN; i++)
    for (int j = i; j > 0 && zam[j].vakit < zam[j - 1].vakit; j--) { Zam t = zam[j]; zam[j] = zam[j - 1]; zam[j - 1] = t; }
  for (int i = 0; i < zamN; i++) {
    char s[24];
    saatYaz(zam[i].vakit, simdi, s, sizeof(s));
    int64_t dk = (zam[i].vakit - simdi + 30) / 60;
    char kalan[24] = "";
    if (dk >= 0 && dk < 120) snprintf(kalan, sizeof(kalan), ", %lld dk sonra", (long long)dk);
    int n = snprintf(out + o, cap - o, "%s#%u %s: %s (%s%s)", i ? "; " : "", zam[i].id, zam[i].tur == ALARM ? "alarm" : "zamanlayıcı",
                     zam[i].etiket, s, kalan);
    if (n < 0 || (size_t)n >= cap - o) break;
    o += (size_t)n;
  }
  return o;
}

int zamanlayiciSayisi() { return zamN; }

void zamanlariGez(void (*f)(void *ctx, int id, char tur, const char *etiket, int64_t vakit), void *ctx) {
  Kilit k;
  int sira[ZAMANLAYICI_MAX], n = zamN;
  for (int i = 0; i < n; i++) sira[i] = i;
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && zam[sira[j]].vakit < zam[sira[j - 1]].vakit; j--) { int t = sira[j]; sira[j] = sira[j - 1]; sira[j - 1] = t; }
  for (int i = 0; i < n; i++) f(ctx, zam[sira[i]].id, zam[sira[i]].tur, zam[sira[i]].etiket, zam[sira[i]].vakit);
}

bool calanVar(int64_t simdi, Calan *c) {
  Kilit k;
  if (!zam) return false;
  int en = -1;
  for (int i = 0; i < zamN; i++)
    if (zam[i].vakit <= simdi && (en < 0 || zam[i].vakit < zam[en].vakit)) en = i;
  if (en < 0) return false;
  c->id = zam[en].id;
  c->tur = zam[en].tur;
  c->vakit = zam[en].vakit;
  c->kacirildi = simdi - zam[en].vakit > 1800;
  memcpy(c->etiket, zam[en].etiket, sizeof(c->etiket));
  memmove(&zam[en], &zam[en + 1], (size_t)(zamN - en - 1) * sizeof(Zam));
  zamN--;
  kirli = true;
  return true;
}

bool degisti() { return kirli; }

bool kaydet() {
  Kilit k;
  if (!kirli || !kayitBuf) return true;
  size_t len = yaziyaDok(kayitBuf, KAYIT_CAP);
  if (!len) return false;
  if (!depo.yaz || !depo.yaz(kayitBuf, len)) return false;
  kirli = false;
  return true;
}

// ---------------------------------------------------------------- kartta: flaşın ffat bölümü
#ifndef NOVA_HOST_TEST
namespace {
const uint32_t YUVA = 32 * 1024, SIHIR = 0x3146484Eu;     // "NHF1"
struct Baslik { uint32_t sihir, sira, uzunluk, crc; };
uint32_t yuvaSira[2] = {0, 0};
int sonYuva = -1;

const esp_partition_t *bolum() {
  static const esp_partition_t *p = nullptr;
  if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "ffat");
  return p;
}

// 2.7.0'da bölüm tablosu değişti (yazılım 3 MB -> 6 MB). 2.6.x tablosunda ffat 0x610000'da başlıyordu;
// yeni tabloda orası ikinci yazılım bölümünün başı. USB yüklemesi yalnızca ilk yazılım bölümünü yazar,
// orayı silmez: yeni depoda kayıt yoksa notlar ve zamanlayıcılar ilk açılışta oradan okunur ve yeni yere
// yazılır. Oraya sonradan kablosuz yükleme yazılırsa ilk baytlar yazılım imzası olur, sihir tutmaz.
const uint32_t ESKI_ADRES = 0x610000;
bool tasindiBayrak = false;

bool eskiYerdenOku(char *buf, size_t cap, size_t *len) {
  const esp_partition_t *p = bolum();
  if (!p || p->address == ESKI_ADRES) return false;   // tablo eski: zaten orası okundu
  Baslik b[2];
  bool ok[2];
  for (int y = 0; y < 2; y++) {
    uint32_t a = ESKI_ADRES + (uint32_t)y * YUVA;
    ok[y] = esp_flash_read(NULL, &b[y], a, sizeof(Baslik)) == ESP_OK && b[y].sihir == SIHIR && b[y].uzunluk > 0 &&
            b[y].uzunluk <= YUVA - sizeof(Baslik) && b[y].uzunluk <= cap;
    if (ok[y] && (esp_flash_read(NULL, buf, a + sizeof(Baslik), b[y].uzunluk) != ESP_OK ||
                  esp_rom_crc32_le(0, (const uint8_t *)buf, b[y].uzunluk) != b[y].crc)) ok[y] = false;
  }
  int sec = ok[0] && ok[1] ? (b[1].sira > b[0].sira ? 1 : 0) : (ok[0] ? 0 : (ok[1] ? 1 : -1));
  if (sec < 0) return false;
  if (esp_flash_read(NULL, buf, ESKI_ADRES + (uint32_t)sec * YUVA + sizeof(Baslik), b[sec].uzunluk) != ESP_OK) return false;
  *len = b[sec].uzunluk;
  return true;
}

bool yuvaOku(int y, Baslik *b) {
  const esp_partition_t *p = bolum();
  if (!p || esp_partition_read(p, (size_t)y * YUVA, b, sizeof(*b)) != ESP_OK) return false;
  return b->sihir == SIHIR && b->uzunluk > 0 && b->uzunluk <= YUVA - sizeof(Baslik);
}

bool flasOku(char *buf, size_t cap, size_t *len) {
  const esp_partition_t *p = bolum();
  if (!p) return false;
  Baslik b[2];
  bool ok[2];
  for (int y = 0; y < 2; y++) {
    ok[y] = yuvaOku(y, &b[y]) && b[y].uzunluk <= cap;
    if (ok[y]) {
      if (esp_partition_read(p, (size_t)y * YUVA + sizeof(Baslik), buf, b[y].uzunluk) != ESP_OK ||
          esp_rom_crc32_le(0, (const uint8_t *)buf, b[y].uzunluk) != b[y].crc) ok[y] = false;
    }
    yuvaSira[y] = ok[y] ? b[y].sira : 0;
  }
  int sec = ok[0] && ok[1] ? (b[1].sira > b[0].sira ? 1 : 0) : (ok[0] ? 0 : (ok[1] ? 1 : -1));
  if (sec < 0) {
    if (!eskiYerdenOku(buf, cap, len)) return false;
    tasindiBayrak = true;
    sonYuva = 1;                       // ilk yazış yeni bölümün 0. yuvasına
    return true;
  }
  if (esp_partition_read(p, (size_t)sec * YUVA + sizeof(Baslik), buf, b[sec].uzunluk) != ESP_OK) return false;
  *len = b[sec].uzunluk;
  sonYuva = sec;
  return true;
}

bool flasTasindi() { return tasindiBayrak; }

bool flasYaz(const char *buf, size_t len) {
  const esp_partition_t *p = bolum();
  if (!p || len > YUVA - sizeof(Baslik)) return false;
  int y = sonYuva == 0 ? 1 : 0;
  size_t silinecek = (sizeof(Baslik) + len + 4095) / 4096 * 4096;
  if (esp_partition_erase_range(p, (size_t)y * YUVA, silinecek) != ESP_OK) return false;
  if (esp_partition_write(p, (size_t)y * YUVA + sizeof(Baslik), buf, len) != ESP_OK) return false;
  Baslik b;
  b.sihir = SIHIR;
  b.sira = (yuvaSira[0] > yuvaSira[1] ? yuvaSira[0] : yuvaSira[1]) + 1;
  b.uzunluk = (uint32_t)len;
  b.crc = esp_rom_crc32_le(0, (const uint8_t *)buf, len);
  if (esp_partition_write(p, (size_t)y * YUVA, &b, sizeof(b)) != ESP_OK) return false;   // başlık en son: yarım kayıt geçersiz kalır
  yuvaSira[y] = b.sira;
  sonYuva = y;
  return true;
}
}  // namespace

Depo flasDepo() {
  Depo d;
  d.oku = flasOku;
  d.yaz = flasYaz;
  d.tasindi = flasTasindi;
  return d;
}

size_t flasDurum(char *out, size_t cap) {
  const esp_partition_t *p = bolum();
  int n = p ? snprintf(out, cap, "depo %u KB (0x%X)%s", (unsigned)(p->size / 1024), (unsigned)p->address,
                       tasindiBayrak ? ", notlar eski yerden taşındı" : "")
            : snprintf(out, cap, "depo bölümü bulunamadı");
  return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}
#endif

}  // namespace hafiza
