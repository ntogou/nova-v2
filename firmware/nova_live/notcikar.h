// Oturum sonu not çıkarma: hafızanın emniyet ağı.
//
// 18 Ekim kaydında ses modeli iki kez "not aldım" dedi ama hafıza aracını çağırmadı (işi arka sisteme
// devretmeden "yaptım" demek GPT-Live'da bilinen bir davranış). Bu yüzden oturum kapanınca kart,
// konuşmanın yazısını ve mevcut notları arka modele (Responses API, gpt-6-luna) kendisi gönderir ve
// "kalıcı olarak ne öğrenildi, ne silinmeli" diye sorar; cevap yapılandırılmış JSON'dur, hafızaya uygulanır.
// Kurallar kişilikteki hafıza kuralıyla aynı (kullanıcının seçtiği üç kademe).
#pragma once
#include <stddef.h>
#include <stdint.h>

// Oturum boyunca biriken konuşma (link.cpp'nin her tam satırı)
void notKonusmaEkle(bool kullanici, const char *metin);
// Oturum kapandı: kullanıcı yeterince konuştuysa konuşma işlenmek üzere sıraya alınır (true).
bool notOturumBitti();
bool notBekliyor();
// Responses API isteğinin gövdesi (masaüstünde sınanır)
size_t notIstekGovdesi(const char *model, const char *notlar, const char *konusma, char *out, size_t cap);
// Responses API cevabını hafızaya uygular. Eklenen + silinen not sayısını döner; anlaşılmadıysa -1.
int notCevabiUygula(const char *json, size_t len, int32_t ymd, char *rapor, size_t cap);
#ifndef NOVA_HOST_TEST
// Sıradaki konuşmayı arka modele sorar ve sonucu uygular (birkaç saniye sürer; şifreli bağlantı).
// true: iş bitti (başarılı ya da vazgeçildi); false: ağ hatası, sonra yeniden denenecek.
bool notCikar(int32_t ymd, char *rapor, size_t cap);
#endif
