// OpenAI GPT-Live mesajları (WebSocket, wss://api.openai.com/v1/live/sessions).
// ChatGPT uygulamasındaki sesli sohbetin modeli: aynı anda dinler ve konuşur.
// Olay ve alan adları developers.openai.com/api/reference/resources/live/primary-websocket ve
// developers.openai.com/api/docs/guides/voice-websockets?api=live sayfalarındaki gibidir (11 Ekim 2026).
// Donanıma bağlı değil.
//
// GPT Realtime'dan (openai.cpp) farkları:
//   - Söz sırası olayı yok: "konuşma başladı / bitti", "cevap bitti" diye haber gelmez. Model
//     ne zaman konuşacağına kendi karar verir; cevabın bittiğini link.cpp ses akışının
//     durmasından anlar.
//   - Giriş sesi kesintisiz akmalı (oturumun saati giriş sesidir). Mikrofon kapalıyken de
//     sessizlik gönderilir.
//   - Modelin kendisi araç kullanmaz. Arama ve ağır düşünme işini ayrı bir "arka model" yapar
//     (delegation). Arka modeli OpenAI çalıştırır; cihaz yalnızca adını verir. Cihazın kendi araçları
//     (ses, parlaklık, hafıza, zamanlayıcı) arka modele "function" olarak verilir: çağrı response.event
//     içinde gelir (response.output_item.done, function_call), sonuç response.item.create ile döner,
//     ardından response.create arka modeli devam ettirir.
//   - Ücret jetonla değil, bağlı kalınan süreyle: oturum açık kaldıkça işler.
//   - Oturum kapanınca geçmiş gider, ama yeni oturuma yazı olarak verilebilir (session.input).
//
// Ayar merdiveni (ModelConfig::level):
//   0: kişilik + ses + geçmiş + arka model (talimat + web araması) + arka model hızlandırma (hızlı kip, düşük düşünme payı)
//   1: aynısı, hızlandırma ayarları olmadan
//   2: kişilik + ses + arka model (araçsız, geçmişsiz)
//   3: yalnızca kişilik (arka model yok; model bir iş devretmek isterse "araç yok" cevabı verilir)
#pragma once
#include "proto.h"

namespace gptlive {

size_t buildSetup(char *out, size_t cap, const ModelConfig &c);                   // session.start
size_t buildAudio(char *out, size_t cap, const int16_t *pcm16k, size_t samples);  // session.input_audio.append (GPTLIVE_AUDIO_RATE 24000 ise 24 kHz'e çevirir)
size_t buildText(char *out, size_t cap, const char *text, int part);              // session.instructions.append (yazılı soru)
size_t buildInterrupt(char *out, size_t cap, int part, uint32_t playedMs);        // session.instructions.append ("sözün kesildi")
size_t buildClose(char *out, size_t cap);                                         // session.close
size_t buildPending(char *out, size_t cap);                                       // cihaza devredilen işe cevap (session.commentary.append)
size_t buildToolResult(char *out, size_t cap, const char *callId, const char *output);  // response.item.create (function_call_output)
size_t buildContinue(char *out, size_t cap);                                      // response.create
size_t buildNote(char *out, size_t cap, const char *text);                        // session.instructions.append (cihazdan not)
bool handle(const char *msg, size_t len, ModelSink &sink);
void reset();
// true: her bağlantıda ilk kez görülen her olay türü ham haliyle bir kez kayda yazılır (sunucunun ne
// yolladığını incelemek için; 2.2.8'de açıktı, protokol görüldükten sonra varsayılan kapalı).
void setFirstEventLog(bool on);

const Provider *provider();

}  // namespace gptlive
