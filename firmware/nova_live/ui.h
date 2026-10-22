// Nova V2 — ekran, arka ışık, pil ölçümü, durum LED'i
// Çizim ekran.cpp'de (A · Bakır Çekirdek); burası veriyi toplar ve kareleri ekrana yollar.
#pragma once
#include <Arduino.h>
#include "config.h"

enum UiState : uint8_t {
  UI_BOOT, UI_WIFI, UI_SERVER, UI_IDLE, UI_LISTEN, UI_THINK, UI_SPEAK, UI_OTA, UI_LOWBAT
};

void uiBegin();
void uiSetState(UiState s);
UiState uiGetState();

// Konuşma yazısı. who: "user" ya da "nova". fresh=true ise önce temizler.
// Nova'nın fresh yazısı ("Mikrofon tamam." gibi) uykuda ve dinlerken birkaç saniye not olarak da görünür.
void uiText(const char *who, const char *text, bool fresh);
void uiClearText();

void uiSetBrightness(int percent);
int  uiGetBrightness();
void uiSetOta(int percent);
void uiSetBootHint(bool on);               // uyandırma sözü kapalı: uykuda "BOOT'a bas" yazar
void uiSetLink(bool wifiUp, int rssi);
void uiSetHava(const char *kisa);          // uyku ekranının alt satırı ("18° parçalı bulut"); "" = yok
void uiSetEtkinlik(const char *etiket);    // düşünürken birkaç saniye görünen iş ("not alıyor")
void uiSetArkaModel(uint32_t basladiMs);   // arka model çalışıyor (millis), 0 = çalışmıyor
void uiScreenOff();
void uiLedOverride(int r, int g, int b);   // durum LED'ini geçici olarak başka işe ver
void uiLedRelease();
size_t uiStatsText(char *out, size_t cap); // çizim ve gönderim süreleri (kayıt için)

float batVolts();     // süzülmüş pil voltajı
int   batPercent();   // 0-100, ölçüm yoksa -1
bool  batIsLow();     // BAT_LOW_V altında 20 saniye kaldı
