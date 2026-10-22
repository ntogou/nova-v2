#include "netlog.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_core_dump.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config.h"
#include "secrets.h"

TeeLog Log;

// Kablosuz yüklenen yazılım, sorunsuz açıldığı anlaşılana kadar (WiFi + sayfa ayakta) "deneme"de kalır;
// o ana kadar kart yeniden başlarsa önyükleyici eski yazılıma döner. Çekirdeğin hazır kontrolü
// (açılışta hemen "sağlam" saymak) bu yüzden devre dışı: netConfirmFirmware() yapar.
extern "C" bool verifyRollbackLater() { return true; }

namespace {

const size_t RING = 64 * 1024;          // kaydın son 64 KB'ı (PSRAM)
const size_t CHUNK = 16 * 1024;         // bir istekte en çok bu kadar
char *ring = nullptr;
char *out = nullptr;
volatile uint32_t head = 0;             // açılıştan beri yazılan toplam bayt
SemaphoreHandle_t mtx = nullptr;

// Yeniden başlamaya dayanan küçük bellek: kaydın son ~2 KB'ı ve art arda çökme sayısı.
// Güç kesilince silinir (o durumda zaten bir sorun yok).
const uint32_t RTC_MAGIC = 0x4E4F5641;  // "NOVA"
const size_t RTC_TAIL = 2000;
struct RtcLog {
  uint32_t magic, pos, len, streak;
  char buf[RTC_TAIL];
};
RTC_NOINIT_ATTR RtcLog rtcLog;
bool rtcOk = false;
int crashStreak = 0;
esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
char *tail = nullptr;                   // önceki çalışmanın son satırları (açılış raporunda yazılıp bırakılır)
size_t tailLen = 0;

WebServer server(80);
bool started = false;
volatile bool otaRunning = false;
int lastPct = -1;
NetOtaHooks ota;

char cmdq[64];                          // sayfadan gelen komutlar (yalnızca loop görevi dokunur)
uint8_t cmdHead = 0, cmdTail = 0;
char question[200];                     // sayfadan yazılan soru
bool questionReady = false;

// tarayıcıdan yükleme
bool upAuth = false, upOk = false, upFirst = false, upFailed = false;
size_t upTotal = 0, upDone = 0;
String upErr;

#ifdef OTA_PASS
const char kPassRaw[] = OTA_PASS;
#else
const char kPassRaw[] = "";
#endif
const char *kPass = nullptr;            // netStart'ta geçerliyse kPassRaw'a bağlanır

// Panel sayfası (A · Bakır Çekirdek): nova_tools/panel.html'den üretilir
#include "panel_sayfa.h"

NetPanelHooks panel;
char *durumBuf = nullptr;                 // /durum cevabı (PSRAM)
const size_t DURUM_CAP = 32 * 1024;

// Durum değiştiren istekler (komut, soru, araç, yükleme) "X-Nova" başlığı ister. Başka bir sitenin
// tarayıcıdaki kayıtlı şifreyle buraya istek attırması böylece engellenir (özel başlık için tarayıcı
// önce izin sorar, kart cevap vermez).
bool sayfadan() {
  if (server.hasHeader("X-Nova")) return true;
  server.send(403, "text/plain; charset=utf-8", "X-Nova başlığı yok");
  return false;
}

bool auth() {
  if (!kPass) return false;
  if (server.authenticate("nova", kPass)) return true;
  server.requestAuthentication(BASIC_AUTH, "Nova");
  return false;
}

void handlePage() {
  if (!auth()) return;
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleLog() {
  if (!auth()) return;
  uint32_t from = server.hasArg("from") ? (uint32_t)strtoul(server.arg("from").c_str(), nullptr, 10) : 0;
  size_t n = 0;
  uint32_t next = from;
  bool skipped = false;
  if (ring && out && xSemaphoreTake(mtx, pdMS_TO_TICKS(100))) {
    uint32_t h = head, oldest = h > RING ? h - (uint32_t)RING : 0;
    if (from > h) from = oldest;                      // kart yeniden başlamış: baştan
    else if (from < oldest) { from = oldest; skipped = true; }
    n = h - from;
    if (n > CHUNK) n = CHUNK;
    size_t at = from % RING, first = n < RING - at ? n : RING - at;
    memcpy(out, ring + at, first);
    if (n > first) memcpy(out + first, ring, n - first);
    next = from + (uint32_t)n;
    xSemaphoreGive(mtx);
  }
  char infoBuf[48];                                   // başlıklarda yalnızca ASCII: sayfa biçimlendirir
  snprintf(infoBuf, sizeof(infoBuf), "%s|%lu|%u", FW_VERSION, (unsigned long)(millis() / 60000),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  server.sendHeader("X-Next", String(next));
  server.sendHeader("X-Info", infoBuf);
  server.sendHeader("Cache-Control", "no-store");
  if (skipped) server.sendHeader("X-Skip", "1");
  server.send_P(200, "text/plain; charset=utf-8", out ? out : "", n);
}

void handleCmd() {
  if (!auth() || !sayfadan()) return;
  String c = server.arg("c");
  for (size_t i = 0; i < c.length(); i++) {
    uint8_t nh = (uint8_t)((cmdHead + 1) % sizeof(cmdq));
    if (nh == cmdTail) break;
    cmdq[cmdHead] = c[i];
    cmdHead = nh;
  }
  server.send(200, "text/plain", "ok");
}

void handleSay() {
  if (!auth() || !sayfadan()) return;
  String t = server.arg("t");
  t.trim();
  if (t.length()) {
    strncpy(question, t.c_str(), sizeof(question) - 1);
    question[sizeof(question) - 1] = 0;
    questionReady = true;
  }
  server.send(200, "text/plain", "ok");
}

void handleDurum() {
  if (!auth()) return;
  server.sendHeader("Cache-Control", "no-store");
  if (!panel.durum) {                   // bakım kipi: panelin verisi yok
    char b[64];
    snprintf(b, sizeof(b), "{\"bakim\":true,\"v\":\"%s\"}", FW_VERSION);
    server.send(200, "application/json; charset=utf-8", b);
    return;
  }
  if (!durumBuf) durumBuf = (char *)heap_caps_malloc(DURUM_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t n = durumBuf ? panel.durum(durumBuf, DURUM_CAP) : 0;
  if (!n) { server.send(503, "text/plain; charset=utf-8", "durum yazılamadı"); return; }
  server.send_P(200, "application/json; charset=utf-8", durumBuf, n);
}

void handleArac() {
  if (!auth() || !sayfadan()) return;
  String ad = server.arg("ad"), args = server.arg("args");
  if (!panel.arac || !ad.length()) { server.send(400, "text/plain; charset=utf-8", "Hata: araç yok"); return; }
  char out[480];
  panel.arac(ad.c_str(), args.c_str(), out, sizeof(out));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", out);
}

void otaBegan(const char *how) {
  otaRunning = true;
  lastPct = -1;
  Log.printf("[ota] %s yükleme başladı: oturum kapatılıyor, uyandırma durduruluyor\n", how);
  if (ota.start) ota.start();
}

void otaProgress(int pct) {
  if (pct / 10 == lastPct / 10) return;
  lastPct = pct;
  Log.printf("[ota] %%%d\n", pct);
  if (ota.progress) ota.progress(pct);
}

void otaFailed(const char *why) {
  if (!otaRunning) return;
  otaRunning = false;
  Log.printf("[ota] HATA: %s. Kart eski yazılımla çalışmaya devam ediyor.\n", why);
  if (ota.end) ota.end(false);
}

// Arduino'nun "Export Compiled Binary" klasöründe iki .bin var: nova_live.ino.bin (uygulama) ve
// nova_live.ino.merged.bin (önyükleyici + bölüm tablosu + uygulama). İkincisi uygulama bölümüne
// yazılırsa kart açılmaz. Uygulama dosyasında 0x20'de uygulama tanımının işareti bulunur.
bool looksLikeApp(const uint8_t *b, size_t n) {
  if (n < 0x24 || b[0] != 0xE9) return false;
  uint32_t magic;
  memcpy(&magic, b + 0x20, 4);
  return magic == 0xABCD5432;
}

void handleUpload() {
  HTTPUpload &u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    upOk = upFailed = false;
    upErr = "";
    upAuth = kPass && server.authenticate("nova", kPass) && server.hasHeader("X-Nova");
    if (!upAuth) return;
    upTotal = server.hasArg("size") ? (size_t)strtoul(server.arg("size").c_str(), nullptr, 10) : 0;
    upDone = 0;
    upFirst = true;
    otaBegan("tarayıcıdan");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) upErr = Update.errorString();
    return;
  }
  if (!upAuth) return;
  if (upErr.length()) {
    if (!upFailed) {
      upFailed = true;
      if (Update.isRunning()) Update.abort();
      otaFailed(upErr.c_str());
    }
    return;
  }
  if (u.status == UPLOAD_FILE_WRITE) {
    if (upFirst) {
      upFirst = false;
      if (!looksLikeApp(u.buf, u.currentSize)) {
        upErr = "bu dosya uygulama değil (build klasöründeki nova_live.ino.bin seçilmeli, merged olan değil)";
        handleUpload();                 // hatayı hemen işle
        return;
      }
    }
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      upErr = Update.errorString();
      handleUpload();
      return;
    }
    upDone += u.currentSize;
    if (upTotal) otaProgress((int)(upDone * 100ULL / upTotal));
  } else if (u.status == UPLOAD_FILE_END) {
    if (Update.end(true)) upOk = true;
    else { upErr = Update.errorString(); handleUpload(); }
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    upErr = "yükleme yarıda kesildi";
    handleUpload();
  }
}

void handleUpdateDone() {
  if (!upAuth) {                        // şifre yoksa 401, başlık yoksa 403
    if (auth()) server.send(403, "text/plain; charset=utf-8", "HATA: X-Nova başlığı yok (yükleme sayfanın kendisinden yapılmalı)");
    return;
  }
  server.sendHeader("Connection", "close");
  if (upOk) {
    server.send(200, "text/plain; charset=utf-8", "tamam: yeni yazılım yazıldı, kart yeniden başlıyor (sayfa birkaç saniyede kendine gelir)");
    Log.println("[ota] yükleme bitti, kart yeniden başlıyor");
    if (ota.progress) ota.progress(100);
    if (ota.end) ota.end(true);
    delay(500);
    ESP.restart();
  }
  if (!upErr.length()) upErr = "dosya gelmedi";
  otaFailed(upErr.c_str());
  server.send(400, "text/plain; charset=utf-8", "HATA: " + upErr + ". Kart eski yazılımla devam ediyor.");
}

const char *resetText(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "güç verildi";
    case ESP_RST_EXT:       return "dış reset";
    case ESP_RST_SW:        return "yazılımla yeniden başlatıldı (yükleme ya da komut)";
    case ESP_RST_PANIC:     return "çökme";
    case ESP_RST_INT_WDT:   return "kesme bekçisi (bir görev takıldı)";
    case ESP_RST_TASK_WDT:  return "görev bekçisi (bir görev takıldı)";
    case ESP_RST_WDT:       return "bekçi zamanlayıcısı";
    case ESP_RST_DEEPSLEEP: return "derin uykudan uyandı";
    case ESP_RST_BROWNOUT:  return "düşük voltaj (pil zayıf ya da ani akım çekişi)";
    case ESP_RST_USB:       return "USB";
    default:                return "bilinmiyor";
  }
}

bool resetIsBad(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT ||
         r == ESP_RST_BROWNOUT || r == ESP_RST_CPU_LOCKUP;
}

// Önceki çalışmanın son satırlarını satır satır yazar
void printTail(const char *t, size_t n) {
  const char *p = t, *end = t + n;
  if (n == RTC_TAIL) {                  // tampon dolmuştu: ilk satır yarım, atla
    const char *nl = (const char *)memchr(p, '\n', n);
    if (nl) p = nl + 1;
  }
  while (p < end) {
    const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
    if (len) Log.printf("[önceki] %.*s\n", (int)len, p);
    p += len + 1;
  }
}

// Çökme anında çekirdek, çöken görevin özetini flaştaki "coredump" bölümüne yazar. Bir kez gösterilip silinir.
void reportCoreDump() {
  if (esp_core_dump_image_check() != ESP_OK) return;
  esp_core_dump_summary_t *s = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
  if (s && esp_core_dump_get_summary(s) == ESP_OK) {
    char why[120] = "";
    if (esp_core_dump_get_panic_reason(why, sizeof(why)) != ESP_OK) why[0] = 0;
    Log.printf("[çökme] kayıtlı çökme: görev \"%s\", PC 0x%08lx, sebep: %s (exccause %lu, adres 0x%08lx)\n", s->exc_task,
               (unsigned long)s->exc_pc, why[0] ? why : "?", (unsigned long)s->ex_info.exc_cause, (unsigned long)s->ex_info.exc_vaddr);
    char b[16 * 11 + 40];
    int n = snprintf(b, sizeof(b), "[çökme] geri izleme%s:", s->exc_bt_info.corrupted ? " (bozuk)" : "");
    for (uint32_t i = 0; i < s->exc_bt_info.depth && i < 16 && n < (int)sizeof(b) - 12; i++)
      n += snprintf(b + n, sizeof(b) - (size_t)n, " 0x%08lx", (unsigned long)s->exc_bt_info.bt[i]);
    Log.println(b);
    Log.println("[çökme] (adresleri çözmek için bu satırlarla aynı derlemenin .elf dosyası gerekir)");
  }
  free(s);
  esp_core_dump_image_erase();
}

// Kablosuz yüklenen bir yazılım açılamayıp eskisine dönüldüyse bir kez söyler.
void reportRollback() {
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  if (!bad) return;
  esp_app_desc_t d;
  if (esp_ota_get_partition_description(bad, &d) != ESP_OK) return;
  char id[48];
  snprintf(id, sizeof(id), "%.16s %.16s", d.date, d.time);
  Preferences p;
  if (!p.begin("netlog", false)) return;
  String seen = p.getString("inv", "");
  if (seen != id) {
    p.putString("inv", id);
    Log.printf("[ota] UYARI: kablosuz yüklenen yazılım (derleme %s) sorunsuz açılamadı; önyükleyici eski yazılıma döndü.\n", id);
  }
  p.end();
}

}  // namespace

size_t TeeLog::write(const uint8_t *buf, size_t n) {
  Serial.write(buf, n);
  if (mtx && n <= RING && xSemaphoreTake(mtx, pdMS_TO_TICKS(20))) {
    if (ring) {
      size_t at = head % RING, first = n < RING - at ? n : RING - at;
      memcpy(ring + at, buf, first);
      if (n > first) memcpy(ring, buf + first, n - first);
      head += (uint32_t)n;
    }
    if (rtcOk) {
      for (size_t i = 0; i < n; i++) {
        rtcLog.buf[rtcLog.pos] = (char)buf[i];
        rtcLog.pos = rtcLog.pos + 1 == RTC_TAIL ? 0 : rtcLog.pos + 1;
      }
      rtcLog.len = rtcLog.len + n > RTC_TAIL ? RTC_TAIL : rtcLog.len + (uint32_t)n;
    }
    xSemaphoreGive(mtx);
  }
  return n;
}

bool netEnabled() { return kPass != nullptr; }
int netCrashStreak() { return crashStreak; }

void netStartedFine() {
  if (rtcOk) rtcLog.streak = 0;
  crashStreak = 0;
}

void netBegin() {
  // 1) Önceki çalışmadan kalanlar (kayıt yazılmadan önce okunmalı)
  esp_reset_reason_t r = esp_reset_reason();
  bool valid = rtcLog.magic == RTC_MAGIC && rtcLog.pos < RTC_TAIL && rtcLog.len <= RTC_TAIL && rtcLog.streak < 1000;
  bool bad = resetIsBad(r);
  bootReason = r;
  if (valid && bad && rtcLog.len) {
    tail = (char *)malloc(rtcLog.len);
    if (tail) {
      tailLen = rtcLog.len;
      size_t start = (rtcLog.pos + RTC_TAIL - rtcLog.len) % RTC_TAIL;
      for (size_t i = 0; i < tailLen; i++) tail[i] = rtcLog.buf[(start + i) % RTC_TAIL];
    }
  }
  crashStreak = bad ? (valid ? (int)rtcLog.streak + 1 : 1) : 0;
  rtcLog.magic = RTC_MAGIC;
  rtcLog.pos = rtcLog.len = 0;
  rtcLog.streak = (uint32_t)crashStreak;
  rtcOk = true;

  // 2) Kayıt tamponu
  mtx = xSemaphoreCreateMutex();
  ring = (char *)heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  out = (char *)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void netBootReport() {
  bool bad = resetIsBad(bootReason);
  Log.printf("[açılış] yeniden başlama sebebi: %s\n", resetText(bootReason));
  if (bad) {
    Log.printf("[açılış] UYARI: önceki çalışma beklenmedik şekilde bitti (art arda %d. kez).%s\n", crashStreak,
               tailLen ? " Son satırları:" : "");
    if (tailLen) printTail(tail, tailLen);
  }
  free(tail);
  tail = nullptr;
  tailLen = 0;
  reportCoreDump();
  reportRollback();
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY)
    Log.println("[ota] bu yazılım kablosuz yüklendi, deneme kipinde: WiFi'ye bağlanıp sayfayı açınca kalıcı olur "
                "(o ana kadar kart yeniden başlarsa eski yazılıma dönülür)");
}

void netConfirmFirmware() {
  static bool done = false;
  if (done) return;
  done = true;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
      Log.println("[ota] yeni yazılım sorunsuz açıldı ve kalıcı oldu");
    else
      Log.println("[ota] UYARI: yeni yazılım kalıcı yapılamadı");
  }
}

void netStart(const NetOtaHooks &hooks) {
  if (started) return;
  started = true;
  if (!kPassRaw[0] || strstr(kPassRaw, "BURAYA")) {
    Log.println("[ağ] kablosuz yükleme ve kayıt sayfası KAPALI: secrets.h'ye kendi şifrenle "
                "#define OTA_PASS \"...\" satırını ekleyip bir kez kabloyla yükle");
    return;
  }
  if (strlen(kPassRaw) < 8) Log.println("[ağ] UYARI: OTA_PASS 8 karakterden kısa; aynı ağdaki biri tahmin edip yazılım yükleyebilir");
  kPass = kPassRaw;
  ota = hooks;
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(kPass);
  ArduinoOTA.onStart([]() { otaBegan("Arduino IDE'den"); });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    otaProgress(total ? (int)(done * 100ULL / total) : 0);
  });
  ArduinoOTA.onEnd([]() {
    Log.println("[ota] yükleme bitti, kart yeniden başlıyor");
    if (ota.end) ota.end(true);
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaFailed(e == OTA_AUTH_ERROR ? "şifre yanlış" : e == OTA_BEGIN_ERROR ? "başlatılamadı (yer yok?)"
              : e == OTA_CONNECT_ERROR ? "bağlantı kurulamadı" : e == OTA_RECEIVE_ERROR ? "veri alınamadı"
              : e == OTA_END_ERROR ? "tamamlanamadı" : "bilinmeyen hata");
  });
  ArduinoOTA.begin();                   // mDNS'i de başlatır: "nova.local"
  server.on("/", handlePage);
  server.on("/log", handleLog);
  server.on("/cmd", handleCmd);
  server.on("/say", handleSay);
  server.on("/durum", handleDurum);
  server.on("/arac", handleArac);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
  static const char *BASLIK[] = {"X-Nova"};
  server.collectHeaders(BASLIK, 1);
  server.begin();
  MDNS.addService("http", "tcp", 80);
  String ip = WiFi.localIP().toString();
  Log.printf("[ağ] panel: http://%s.local (%s)\n", HOSTNAME, ip.c_str());
}

void netSetPanel(const NetPanelHooks &h) { panel = h; }

void netLoop() {
  if (!kPass) return;
  ArduinoOTA.handle();
  if (!otaRunning) server.handleClient();
}

int netReadCommand() {
  if (cmdHead == cmdTail) return -1;
  char c = cmdq[cmdTail];
  cmdTail = (uint8_t)((cmdTail + 1) % sizeof(cmdq));
  return (unsigned char)c;
}

bool netReadQuestion(char *buf, size_t cap) {
  if (!questionReady || !cap) return false;
  questionReady = false;
  strncpy(buf, question, cap - 1);
  buf[cap - 1] = 0;
  return true;
}
