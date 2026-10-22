#include "hava.h"
#include "hafiza.h"
#include "config.h"
#include "psbuf.h"
#include "src/ArduinoJson-v7.4.2.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifndef NOVA_HOST_TEST
#include <Arduino.h>
#include "agistek.h"
#endif

namespace {

const size_t OZET_CAP = 700;
char *ozetBuf = nullptr;         // OZET_CAP bayt, PSRAM'de
uint32_t alindi = 0;
bool var = false;

// WMO hava kodu (Open-Meteo weather_code)
const char *durumAdi(int k) {
  switch (k) {
    case 0: return "açık";
    case 1: return "çoğunlukla açık";
    case 2: return "parçalı bulutlu";
    case 3: return "kapalı";
    case 45: case 48: return "sisli";
    case 51: case 53: case 55: return "çiseleyen yağmurlu";
    case 56: case 57: return "dondurucu çisentili";
    case 61: return "hafif yağmurlu";
    case 63: return "yağmurlu";
    case 65: return "kuvvetli yağmurlu";
    case 66: case 67: return "dondurucu yağmurlu";
    case 71: return "hafif karlı";
    case 73: return "karlı";
    case 75: return "yoğun karlı";
    case 77: return "kar taneli";
    case 80: return "hafif sağanaklı";
    case 81: return "sağanak yağışlı";
    case 82: return "şiddetli sağanaklı";
    case 85: case 86: return "kar sağanaklı";
    case 95: return "gök gürültülü fırtınalı";
    case 96: case 99: return "dolulu fırtınalı";
  }
  return "değişken";
}

const char *gunAdi(const char *tarih) {
  static const char *const G[7] = {"Pazar", "Pazartesi", "Salı", "Çarşamba", "Perşembe", "Cuma", "Cumartesi"};
  int y = 0, m = 0, d = 0;
  if (!tarih || sscanf(tarih, "%d-%d-%d", &y, &m, &d) != 3) return "";
  int wd = 0;
  hafiza::yerelTakvim(hafiza::yerelEpoch(y, m, d, 12, 0), nullptr, nullptr, nullptr, nullptr, nullptr, &wd);
  return G[wd % 7];
}

int yuvarla(float v) { return (int)lroundf(v); }

}  // namespace

bool havaCoz(const char *json, size_t len, const char *yer, char *out, size_t cap) {
  JsonDocument d;
  if (deserializeJson(d, json, len)) return false;
  JsonObject c = d["current"];
  JsonObject g = d["daily"];
  if (c.isNull() || g.isNull() || !c["temperature_2m"].is<float>()) return false;
  const char *saat = c["time"] | "";
  const char *hhmm = strchr(saat, 'T');
  hhmm = hhmm ? hhmm + 1 : "";
  size_t o = 0;
  int n = snprintf(out, cap, "Hava durumu (%s, saat %.5s itibarıyla): şu an %d°C (hissedilen %d), %s, nem %%%d, rüzgâr %d km/sa.",
                   yer, hhmm, yuvarla(c["temperature_2m"] | 0.0f), yuvarla(c["apparent_temperature"] | 0.0f),
                   durumAdi(c["weather_code"] | -1), (int)(c["relative_humidity_2m"] | 0), yuvarla(c["wind_speed_10m"] | 0.0f));
  if (n < 0 || (size_t)n >= cap) return false;
  o = (size_t)n;
  JsonArray tt = g["time"], kod = g["weather_code"], mx = g["temperature_2m_max"], mn = g["temperature_2m_min"],
            yg = g["precipitation_probability_max"];
  for (size_t i = 0; i < tt.size() && i < 3; i++) {
    const char *ad = i == 0 ? "Bugün" : (i == 1 ? "Yarın" : nullptr);
    char baslik[32];
    if (ad) snprintf(baslik, sizeof(baslik), "%s", ad);
    else snprintf(baslik, sizeof(baslik), "%s", gunAdi(tt[i] | ""));
    char yagis[40] = "";
    if (yg[i].is<int>()) snprintf(yagis, sizeof(yagis), ", yağış ihtimali %%%d", yg[i].as<int>());
    n = snprintf(out + o, cap - o, " %s: %d–%d°C, %s%s.", baslik, yuvarla(mn[i] | 0.0f), yuvarla(mx[i] | 0.0f),
                 durumAdi(kod[i] | -1), yagis);
    if (n < 0 || (size_t)n >= cap - o) break;
    o += (size_t)n;
  }
  return true;
}

void havaSakla(const char *ozet, uint32_t simdiMs) {
  if (!psLazy(ozetBuf, OZET_CAP)) return;
  strncpy(ozetBuf, ozet, OZET_CAP - 1);
  ozetBuf[OZET_CAP - 1] = 0;
  alindi = simdiMs ? simdiMs : 1;
  var = true;
}

size_t havaYazi(char *out, size_t cap, uint32_t simdiMs) {
  if (!cap) return 0;
  out[0] = 0;
  if (!var || simdiMs - alindi > HAVA_ESKI_MS) return 0;
  int n = snprintf(out, cap, "%s", ozetBuf);
  return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t havaKisa(char *out, size_t cap, uint32_t simdiMs) {
  if (!cap) return 0;
  out[0] = 0;
  if (!var || simdiMs - alindi > HAVA_ESKI_MS) return 0;
  // Özetten okunur: "... şu an 18°C (hissedilen 17), parçalı bulutlu, nem ..."
  const char *p = strstr(ozetBuf, "şu an ");
  if (!p) return 0;
  p += strlen("şu an ");
  char *son;
  long derece = strtol(p, &son, 10);
  if (son == p) return 0;
  const char *d = strstr(son, "), ");
  char durum[40] = "";
  if (d) {
    d += 3;
    const char *v = strchr(d, ',');
    size_t n = v ? (size_t)(v - d) : strlen(d);
    if (n >= sizeof(durum)) n = sizeof(durum) - 1;
    memcpy(durum, d, n);
    durum[n] = 0;
  }
  // Uzun adların ekrana sığan kısaltması (durumAdi ile aynı adlar)
  static const char *const K[][2] = {
    {"çoğunlukla açık", "az bulutlu"}, {"parçalı bulutlu", "parçalı bulut"},
    {"çiseleyen yağmurlu", "çisenti"}, {"dondurucu çisentili", "çisenti"},
    {"hafif yağmurlu", "hafif yağmur"}, {"yağmurlu", "yağmur"}, {"kuvvetli yağmurlu", "kuvvetli yağmur"},
    {"dondurucu yağmurlu", "donan yağmur"}, {"hafif karlı", "hafif kar"}, {"karlı", "kar"},
    {"yoğun karlı", "yoğun kar"}, {"kar taneli", "kar"}, {"hafif sağanaklı", "hafif sağanak"},
    {"sağanak yağışlı", "sağanak"}, {"şiddetli sağanaklı", "sağanak"}, {"kar sağanaklı", "kar"},
    {"gök gürültülü fırtınalı", "fırtına"}, {"dolulu fırtınalı", "dolu"}, {"değişken", ""},
  };
  const char *kisa = durum;
  for (auto &e : K)
    if (!strcmp(durum, e[0])) { kisa = e[1]; break; }
  int n = snprintf(out, cap, *kisa ? "%ld° %s" : "%ld°", derece, kisa);
  if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
  return (size_t)n;
}

uint32_t havaAlindiMs() { return var ? alindi : 0; }

#ifndef NOVA_HOST_TEST
bool havaCek(char *hata, size_t cap) {
  char yol[360];
  snprintf(yol, sizeof(yol),
           "/v1/forecast?latitude=%.2f&longitude=%.2f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=Europe%%2FIstanbul&forecast_days=3",
           (double)HAVA_ENLEM, (double)HAVA_BOYLAM);
  const size_t CAP = 6144;
  static char *g = nullptr;
  if (!psLazy(g, CAP)) { snprintf(hata, cap, "bellek yok"); return false; }
  char *govde = nullptr;
  size_t gl = 0;
  int kod = httpsIstek("api.open-meteo.com", "GET", yol, nullptr, nullptr, 0, g, CAP, &govde, &gl, 10000, hata, cap);
  if (kod < 0) return false;
  if (kod != 200) { snprintf(hata, cap, "HTTP %d", kod); return false; }
  char ozet[OZET_CAP];
  if (!havaCoz(govde, gl, HAVA_YER, ozet, sizeof(ozet))) {
    snprintf(hata, cap, "cevap anlaşılmadı (%u bayt)", (unsigned)gl);
    return false;
  }
  havaSakla(ozet, millis());
  return true;
}
#endif
