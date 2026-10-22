// Masaüstü testi: bağlantı mantığı (link.cpp + gptlive.cpp) sahte bir GPT-Live sunucusuyla.
// Sunucu süreç içinde sahte: WebSocket yükseltmesini kabul eder, "session.start"a "session.started" der,
// gelen ses ve yazı mesajlarını sayar. Zaman sahte (fakeNow), her şey tek iş parçacığında.
#include "link.h"
#include "config.h"
#include "audio.h"
#include <string>
#include <vector>

uint32_t fakeNow = 1000;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  !! BASARISIZ (%s:%d): ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Mesajdaki ses alanının (base64) içinde sıfır olmayan bayt var mı: mikrofon sesi mi, sessizlik mi
static bool audioNonZero(const std::string &m) {
  size_t p = m.find("\"data\":\"");
  if (p == std::string::npos) p = m.find("\"audio\":\"");
  if (p == std::string::npos) return false;
  p = m.find(':', p) + 2;
  size_t e = m.find('"', p);
  for (size_t i = p; i < e; i++) if (m[i] != 'A' && m[i] != '=') return true;   // "AAAA" = sıfır bayt
  return false;
}

struct FakeServer : WsTransport {
  bool failOpen = false, conn = false, upgraded = false, setupSeen = false;
  std::string req, in, rx, lastSetup;
  size_t rpos = 0;
  int opens = 0, audioMsgs = 0, setups = 0;
  uint32_t firstAudioAt = 0;
  int pings = 0;
  std::vector<std::string> texts;
  void push(const std::string &msg) {
    std::string f;
    f += (char)0x81;
    if (msg.size() < 126) f += (char)msg.size();
    else { f += (char)126; f += (char)(msg.size() >> 8); f += (char)(msg.size() & 255); }
    rx += f + msg;
  }
  void onMessage(const std::string &m) {
    if (!setupSeen) {
      setupSeen = true;
      setups++;
      lastSetup = m;
      CHECK(m.find("\"session.start\"") != std::string::npos, "ilk mesaj session.start degil: %.120s", m.c_str());
      push("{\"type\":\"session.started\",\"session\":{\"id\":\"s1\"}}");
      return;
    }
    if (m.find("\"type\":\"session.close\"") != std::string::npos) {
      push("{\"type\":\"session.closed\",\"reason\":\"close_requested\",\"usage\":{\"seconds\":3}}");
      return;
    }
    if (m.find("session.input_audio.append") != std::string::npos) {
      if (audioNonZero(m)) {                 // mikrofon sesi (GPT-Live'da kapalıyken sessizlik de gider)
        if (!audioMsgs) firstAudioAt = fakeNow;
        audioMsgs++;
      }
      return;
    }
    texts.push_back(m);
  }
  void parse() {
    for (;;) {
      if (in.size() < 2) return;
      const uint8_t *b = (const uint8_t *)in.data();
      uint8_t op = b[0] & 15;
      uint64_t len = b[1] & 127;
      size_t p = 2;
      if (len == 126) { if (in.size() < 4) return; len = (b[2] << 8) | b[3]; p = 4; }
      else if (len == 127) { if (in.size() < 10) return; len = 0; for (int i = 0; i < 8; i++) len = (len << 8) | b[2 + i]; p = 10; }
      if (in.size() < p + 4 + len) return;
      const uint8_t *mask = b + p;
      std::string pay(len, 0);
      for (uint64_t i = 0; i < len; i++) pay[i] = (char)(b[p + 4 + i] ^ mask[i & 3]);
      in.erase(0, p + 4 + len);
      if (op == 1) onMessage(pay);
      else if (op == 8) conn = false;
      else if (op == 9) { pings++; rx += std::string("\x8a\x00", 2); }   // ping -> pong
    }
  }
  bool open(const char *, uint16_t, uint32_t) override {
    opens++;
    if (failOpen) return false;
    conn = true; upgraded = false; setupSeen = false; req.clear(); in.clear(); rx.clear(); rpos = 0;
    return true;
  }
  void close() override { conn = false; }
  bool connected() override { return conn; }
  int available() override { return (int)(rx.size() - rpos); }
  int read(uint8_t *buf, size_t n) override {
    size_t k = rx.size() - rpos < n ? rx.size() - rpos : n;
    memcpy(buf, rx.data() + rpos, k);
    rpos += k;
    return (int)k;
  }
  bool write(const uint8_t *buf, size_t n) override {
    if (!conn) return false;
    if (!upgraded) {
      req.append((const char *)buf, n);
      if (req.find("\r\n\r\n") != std::string::npos) {
        upgraded = true;
        rx += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
      }
      return true;
    }
    in.append((const char *)buf, n);
    parse();
    return true;
  }
};

static FakeServer srv;
static bool armed = false, wakeTrigger = false, armedAtConnect = true;
static int armCalls = 0, disarmCalls = 0, feeds = 0, chimes = 0, connects = 0;
static uint32_t chimeAt = 0, lastArmAt = 0;

static bool fakeWakeArm(bool on) {
  if (on) { armCalls++; armed = true; lastArmAt = fakeNow; return true; }
  disarmCalls++; armed = false; return false;
}
static bool fakeWakeFeed(const int16_t *, size_t n) {
  CHECK(armed, "motor kurulu degilken ses verildi");
  CHECK(n == MIC_FRAME_SAMPLES, "cerceve boyu %zu", n);
  feeds++;
  if (wakeTrigger) { wakeTrigger = false; return true; }
  return false;
}
static uint32_t fakeChime() { chimes++; chimeAt = fakeNow; return 140; }
static void fakeBeforeConnect() { armedAtConnect = armed; connects++; }
static uint32_t hookNow() { return fakeNow; }
static uint32_t hookRnd() { return 12345; }
static void hookIdle() { fakeNow++; }
static bool bigSys = false;
static void hookSys(char *b, size_t cap) {
  if (!bigSys) { snprintf(b, cap, "Adın Nova."); return; }
  // En büyük talimat (2.7.0): kişilik + kişisel profil ~14 KB, dolu hafıza ~11,5 KB, gündem, hava, zamanlayıcılar: ~31 KB
  size_t o = 0;
  while (o + 200 < 31000 && o + 200 < cap) o += (size_t)snprintf(b + o, cap - o, "- (18.10) Kullanıcı hakkında uzunca bir not, \"tırnaklı\" ve Türkçe harfli: şğüöçı.\n");
  snprintf(b + o, cap - o, "SON-ISARET");
}
static void *hookAlloc(size_t n) { return malloc(n); }

static void run(uint32_t ms) {
  uint32_t end = fakeNow + ms;
  while ((int32_t)(fakeNow - end) < 0) {
    linkLoop();
    fakeNow += linkState() == LINK_IDLE ? 20 : 3;
  }
}
static bool runUntil(LinkState s, uint32_t ms) {
  uint32_t end = fakeNow + ms;
  while ((int32_t)(fakeNow - end) < 0) {
    linkLoop();
    if (linkState() == s) return true;
    fakeNow += linkState() == LINK_IDLE ? 20 : 3;
  }
  return linkState() == s;
}
static bool logHas(const char *s) { return Serial.all.find(s) != std::string::npos; }
static size_t logCount(const char *s) {
  size_t n = 0;
  for (size_t p = 0; (p = Serial.all.find(s, p)) != std::string::npos; p++) n++;
  return n;
}

// Ücret sayacı kancası
static int usageCalls = 0;
static bool usageSpoke = false;
static float usageSec = 0;
static void fakeUsage(float sec, bool spoke) { usageCalls++; usageSpoke = spoke; usageSec = sec; }


// GPT-Live'dan gelen sesli cevap: yüksek seviyeli 24 kHz PCM, base64
static std::string b64(const std::string &in) {
  static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8) | (uint8_t)in[i + 2];
    o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
  }
  if (i + 1 == in.size()) { uint32_t v = (uint8_t)in[i] << 16; o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "=="; }
  else if (i + 2 == in.size()) { uint32_t v = ((uint8_t)in[i] << 16) | ((uint8_t)in[i + 1] << 8); o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '='; }
  return o;
}
static std::string loudAudio(int ms) {
  std::string pcm;
  for (int i = 0; i < 24 * ms; i++) { int16_t v = (i / 10) & 1 ? 4000 : -4000; pcm += (char)(v & 255); pcm += (char)((v >> 8) & 255); }
  return b64(pcm);
}
// GPT-Live model susarken de akışı sürdürür: düşük seviyeli (hışırtı) parça
static std::string silentAudio(int ms) {
  std::string pcm;
  for (int i = 0; i < 24 * ms; i++) { int16_t v = (i & 1) ? 12 : -12; pcm += (char)(v & 255); pcm += (char)((v >> 8) & 255); }
  return b64(pcm);
}
// Kullanıcının sözü (yazısı) ve ardından Nova'nın sesli cevabı (yazısıyla)
static void liveTurn(const char *user, const char *nova) {
  srv.push(std::string("{\"type\":\"session.input_transcript.delta\",\"delta\":\"") + user + "\"}");
  run(300);
  srv.push(std::string("{\"type\":\"session.output_transcript.delta\",\"delta\":\"") + nova + "\"}");
  srv.push("{\"type\":\"session.output_audio.delta\",\"delta\":\"" + loudAudio(300) + "\"}");
}

// Cihaz araçları ve duyulan söz kancaları
static int toolRuns = 0;
static std::string lastToolName, lastToolArgs, lastHeard;
static void fakeTool(const char *name, const char *args, char *out, size_t cap) {
  toolRuns++;
  lastToolName = name;
  lastToolArgs = args;
  snprintf(out, cap, "Ses %%30 yapıldı.");
}
static void fakeHeard(const char *t) { lastHeard = t; }
static std::string lines;
static void fakeLine(bool u, const char *t) { lines += u ? "S:" : "N:"; lines += t; lines += "|"; }
static void fakeBackend(char *b, size_t cap) { snprintf(b, cap, "Arka model talimatı (test)."); }
static const char TEST_TOOLS[] = "[{\"type\":\"function\",\"name\":\"set_volume\",\"description\":\"ses\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}]";
static size_t textsWith(const char *k, size_t from = 0) {
  size_t n = 0;
  for (size_t i = from; i < srv.texts.size(); i++) if (srv.texts[i].find(k) != std::string::npos) n++;
  return n;
}

// Yankı silme kancaları (araya girme)
extern int spkFlushes;
extern size_t spkBytes;
static bool echoOn = false, echoWorks = true;
static int echoOnCalls = 0, echoOffCalls = 0;
static void fakeEchoCancel(bool on) { if (on) echoOnCalls++; else echoOffCalls++; echoOn = on; }
static bool fakeEchoActive() { return echoOn && echoWorks; }

// Nova ms boyunca konuşur (100 ms'de bir sesli parça); bu sırada isteğe bağlı olarak kullanıcının sözü gelir
static void novaSpeaks(int ms, const char *userText = nullptr, int userAt = 500) {
  for (int t = 0; t < ms; t += 100) {
    srv.push("{\"type\":\"session.output_audio.delta\",\"delta\":\"" + loudAudio(100) + "\"}");
    if (userText && t == userAt)
      srv.push(std::string("{\"type\":\"session.input_transcript.delta\",\"delta\":\"") + userText + "\"}");
    run(100);
  }
}

int main() {
  LinkHooks h;
  h.nowMs = hookNow; h.rnd = hookRnd; h.idle = hookIdle; h.systemText = hookSys; h.bigAlloc = hookAlloc;
  h.wakeArm = fakeWakeArm; h.wakeFeed = fakeWakeFeed; h.readyChime = fakeChime; h.beforeConnect = fakeBeforeConnect;
  h.usage = fakeUsage;
  h.echoCancel = fakeEchoCancel; h.echoActive = fakeEchoActive;
  h.toolsJson = TEST_TOOLS; h.tool = fakeTool; h.heard = fakeHeard; h.line = fakeLine; h.backendText = fakeBackend;
  CHECK(linkBegin(&srv, h), "linkBegin");
  linkSetNetUp(true);
  linkSetMicOk(true);

  printf("== 1) uykuda motor kurulur ve ses alır\n");
  run(1000);
  CHECK(armed && armCalls == 1, "kurulmadi (kurulum %d)", armCalls);
  CHECK(feeds >= 10 && feeds <= 14, "1 sn'de %d cerceve verildi", feeds);

  printf("== 2) \"Hey Nova\" -> motor kalkar, bağlanır, hazır sesi, mikrofon sesten sonra açılır\n");
  wakeTrigger = true;
  CHECK(runUntil(LINK_LISTEN, 3000), "dinlemeye gecmedi (durum %s)", linkStateName());
  CHECK(logHas("\"Hey Nova\" duyuldu (açılıştan beri 1. kez)"), "uyanma satiri yok");
  CHECK(!armedAtConnect && connects == 1 && disarmCalls == 1, "baglanirken motor kurulu (bagl %d, kaldirma %d)", connects, disarmCalls);
  CHECK(srv.setups == 1 && chimes == 1, "kurulum %d, hazir sesi %d", srv.setups, chimes);
  int feedsAtListen = feeds;
  run(1000);
  CHECK(srv.audioMsgs > 0 && srv.firstAudioAt >= chimeAt + 190, "ilk ses %u ms sonra gitti (hazir sesi %u)", srv.firstAudioAt - chimeAt, chimeAt);
  CHECK(feeds == feedsAtListen && !armed, "oturumdayken motora ses verildi");

  printf("== 3) sessizlikte oturum kapanır, motor %d ms bekleyip yeniden kurulur\n", WAKE_HOLDOFF_MS);
  CHECK(runUntil(LINK_IDLE, LINK_IDLE_SLEEP_MS + 5000), "uyumadi");
  uint32_t idleAt = fakeNow;
  int a0 = armCalls;
  run(WAKE_HOLDOFF_MS + 200);
  CHECK(armCalls == a0 + 1 && lastArmAt >= idleAt + WAKE_HOLDOFF_MS - 30 && lastArmAt <= idleAt + WAKE_HOLDOFF_MS + 60,
        "yeniden kurulum %d ms sonra (kurulum %d)", (int)(lastArmAt - idleAt), armCalls - a0);

  printf("== 4) 'n' kapalı: motor kalkar, ses verilmez; açınca yeniden kurulur\n");
  linkSetWakeEnabled(false);
  run(300);
  int f0 = feeds;
  run(1000);
  CHECK(!armed && feeds == f0, "kapaliyken motor kurulu ya da ses verildi");
  linkSetWakeEnabled(true);
  linkWakeRearm();
  run(300);
  CHECK(armed, "acinca kurulmadi");

  printf("== 5) yeniden kur (ayar değişti): kaldırılıp hemen kurulur\n");
  int d0 = disarmCalls; a0 = armCalls;
  linkWakeRearm();
  run(200);
  CHECK(disarmCalls == d0 + 1 && armCalls == a0 + 1 && armed, "yeniden kurulmadi (%d/%d)", disarmCalls - d0, armCalls - a0);

  printf("== 6) bakım (kablosuz yükleme): oturum kapanır, uyandırma ve bağlanma durur\n");
  wakeTrigger = true;
  CHECK(runUntil(LINK_LISTEN, 3000), "ikinci uyanma olmadi");
  linkSetHold(true);
  CHECK(runUntil(LINK_IDLE, 500), "bakimda oturum kapanmadi");
  CHECK(logHas("bakım (kablosuz yükleme): oturum kapatıldı"), "bakim satiri yok");
  run(WAKE_HOLDOFF_MS + 500);
  CHECK(!armed, "bakimda motor kuruldu");
  int c0 = connects;
  linkWake();
  linkSay("merhaba");
  run(2000);
  CHECK(linkState() == LINK_IDLE && connects == c0, "bakimda baglanildi");
  linkSetHold(false);
  run(200);
  CHECK(armed, "bakim bitince motor kurulmadi");

  printf("== 7) bağlantı kurulamazsa uykuya dönülür, motor yeniden kurulur\n");
  srv.failOpen = true;
  wakeTrigger = true;
  run(200);
  CHECK(linkState() == LINK_CONNECTING, "baglanmaya baslamadi (%s)", linkStateName());
  CHECK(runUntil(LINK_IDLE, 120000), "baglanamayinca uykuya donmedi");
  idleAt = fakeNow;
  run(WAKE_HOLDOFF_MS + 200);
  CHECK(armed && lastArmAt >= idleAt + WAKE_HOLDOFF_MS - 30, "basarisiz baglantidan sonra kurulmadi");
  srv.failOpen = false;

  printf("== 8) mikrofon yoksa motor kurulmaz\n");
  linkSetMicOk(false);
  run(300);
  f0 = feeds;
  run(1000);
  CHECK(!armed && feeds == f0, "mikrofonsuz motor calisti");
  linkSetMicOk(true);
  run(300);
  CHECK(armed, "mikrofon gelince kurulmadi");

  printf("== 9) sayfadan yazılı soru: bağlanır ve yazıyı gönderir\n");
  size_t t0 = srv.texts.size();
  linkSay("Bugün hava nasıl?");
  CHECK(runUntil(LINK_LISTEN, 3000) || linkState() == LINK_THINK, "yazili soruda baglanmadi");
  run(500);
  bool sent = false;
  for (size_t i = t0; i < srv.texts.size(); i++) if (srv.texts[i].find("Bugün hava nasıl?") != std::string::npos) sent = true;
  CHECK(sent, "soru sunucuya gitmedi (%zu mesaj)", srv.texts.size() - t0);

  printf("== 10) ses tonu: seçilen ses sonraki oturumun kurulumunda gider, boş seçim varsayılana döner\n");
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  linkSetVoice("cedar");
  CHECK(!strcmp(linkVoiceName(), "cedar"), "ses adi");
  linkSay("Yeni sesinle kendini tanıt.");
  CHECK(runUntil(LINK_LISTEN, 4000), "yeni sesle baglanmadi");
  CHECK(srv.lastSetup.find("\"voice\":\"cedar\"") != std::string::npos, "kurulumda ses yok: %.200s", srv.lastSetup.c_str());
  CHECK(logHas("ses cedar, ayar basamağı"), "baglanti satirinda ses yok");
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  linkSetVoice(nullptr);
  CHECK(!strcmp(linkVoiceName(), GPTLIVE_VOICE), "varsayilan ses adi");
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "varsayilan sesle baglanmadi");
  CHECK(srv.lastSetup.find("\"voice\":\"" GPTLIVE_VOICE "\"") != std::string::npos, "varsayilan ses gitmedi");

  printf("== 11) kullanıcının sözü listede yok ama Nova \"görüşürüz\" dedi -> oturum kapanır, sayaç işler\n");
  run(1500);
  CHECK(linkState() == LINK_LISTEN, "oturum dinlemiyor (%s)", linkStateName());
  int u0 = usageCalls;
  liveTurn("Kapatır mısın sesyonu", "Tabii, görüşürüz.");
  CHECK(runUntil(LINK_IDLE, 5000), "Nova vedalasti ama oturum kapanmadi (%s)", linkStateName());
  CHECK(logHas("Nova vedalaştı"), "Nova veda satiri yok");
  CHECK(usageCalls == u0 + 1 && usageSpoke && usageSec > 2.5f, "sayac: cagri %d, konustu %d, %.1f sn", usageCalls - u0, usageSpoke, usageSec);

  printf("== 12) Nova birine selam iletirken \"kendine iyi bak\" der: oturum kapanmaz\n");
  run(WAKE_HOLDOFF_MS + 300);
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  run(1500);
  size_t v0 = logCount("Nova vedalaştı");
  liveTurn("Kardeşime selam verir misin", "Selam, kendine iyi bak.");
  run(4000);
  CHECK(linkState() != LINK_IDLE && logCount("Nova vedalaştı") == v0, "selamda oturum kapandi (%s)", linkStateName());
  CHECK(logHas("[nova] Selam, kendine iyi bak."), "Nova satiri yok");

  printf("== 13) konuşmasız oturum sayaçta ayrı sayılır\n");
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  run(WAKE_HOLDOFF_MS + 300);
  u0 = usageCalls;
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  run(1000);
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  CHECK(usageCalls == u0 + 1 && !usageSpoke, "konusmasiz oturum: cagri %d, konustu %d", usageCalls - u0, usageSpoke);

  printf("== 14) araya girme kapalı (varsayılan): Nova konuşurken modele yalnızca sessizlik gider, yankı silme kurulmaz\n");
  CHECK(!linkBargeIn(), "varsayilan acik");
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  run(1000);
  CHECK(echoOnCalls == 0, "kapaliyken yanki silme istendi");
  novaSpeaks(300);
  CHECK(linkState() == LINK_SPEAK, "Nova konusmuyor (%s)", linkStateName());
  a0 = srv.audioMsgs;
  int fl0 = spkFlushes;
  novaSpeaks(1000, "dur bir saniye");
  CHECK(srv.audioMsgs == a0, "kapaliyken konusma sirasinda mikrofon gitti (%d)", srv.audioMsgs - a0);
  CHECK(spkFlushes == fl0 && !logHas("araya girdin"), "kapaliyken soz kesildi");
  run(3000);
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");

  printf("== 15) araya girme açık: yankı silme kurulur, Nova konuşurken mikrofon gider, söz yazıya dökülünce Nova susar\n");
  linkSetBargeIn(true);
  run(WAKE_HOLDOFF_MS + 300);
  int on0 = echoOnCalls, off0 = echoOffCalls;
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  run(1000);
  CHECK(echoOnCalls == on0 + 1 && echoOn, "oturumda yanki silme istenmedi");
  CHECK(logHas("araya girme açık"), "araya girme satiri yok");
  novaSpeaks(300);
  CHECK(linkState() == LINK_SPEAK, "Nova konusmuyor (%s)", linkStateName());
  a0 = srv.audioMsgs;
  run(400);
  CHECK(srv.audioMsgs > a0, "Nova konusurken mikrofon gitmedi");
  fl0 = spkFlushes;
  novaSpeaks(1500, "dur bir saniye", 1200);            // cevabın başından ~1,9 sn sonra: araya girme
  CHECK(spkFlushes > fl0 && logHas("araya girdin"), "soz kesilmedi (flush %d)", spkFlushes - fl0);
  CHECK(linkState() == LINK_LISTEN || linkState() == LINK_THINK, "kesildikten sonra dinlemiyor (%s)", linkStateName());
  size_t b0 = spkBytes;
  novaSpeaks(300);                                     // turun gecikmeli gelen sesi atılmalı
  CHECK(spkBytes == b0, "kesilen turun sesi calindi (%zu bayt)", spkBytes - b0);
  run(1500);
  novaSpeaks(500);                                     // yeni cevap: yeniden çalınır
  CHECK(spkBytes > b0 && linkState() == LINK_SPEAK, "yeni cevap calinmadi");
  // Cevabın ilk anlarında gelen yazı kullanıcının önceki sözünün geç parçasıdır: kesmez (17 Ekim'deki hata)
  fl0 = spkFlushes;
  size_t cuts0 = logCount("araya girdin");
  novaSpeaks(600, "ın", 200);
  CHECK(spkFlushes == fl0 && logCount("araya girdin") == cuts0 && linkState() == LINK_SPEAK,
        "cevabin basinda gelen gec yazi cevabi kesti");
  // Konuşma başlamadan önceki cümlenin parçaları 1,5 sn'yi aşsa da (araları <1,2 sn) cevabı kesmez ("Kestin dimi sen az" + " önce", 2.8.2)
  novaSpeaks(700, " sen az", 400);
  novaSpeaks(600, " önce", 300);
  CHECK(spkFlushes == fl0 && logCount("araya girdin") == cuts0 && linkState() == LINK_SPEAK,
        "devam eden gec kullanici yazisi cevabi kesti");
  run(3000);
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  CHECK(echoOffCalls > off0 && !echoOn, "oturum kapaninca yanki silme kaldirilmadi");

  printf("== 16) araya girme açık ama yankı silme kurulamadı: eski davranış (sessizlik, kesme yok)\n");
  echoWorks = false;
  run(WAKE_HOLDOFF_MS + 300);
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  run(1000);
  novaSpeaks(300);
  a0 = srv.audioMsgs; fl0 = spkFlushes;
  novaSpeaks(1000, "dur bir saniye");
  CHECK(srv.audioMsgs == a0 && spkFlushes == fl0, "yanki silme yokken mikrofon gitti ya da soz kesildi");
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  echoWorks = true;
  linkSetBargeIn(false);

  printf("== 17) araç: kurulumda araçlar gider; arka modelin çağrısı yürütülür, sonuç ve devam mesajı gider\n");
  run(WAKE_HOLDOFF_MS + 300);
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
  CHECK(srv.lastSetup.find("\"name\":\"set_volume\"") != std::string::npos && srv.lastSetup.find("\"web_search\"") != std::string::npos &&
        srv.lastSetup.find("\"parallel_tool_calls\":false") != std::string::npos &&
        srv.lastSetup.find("Arka model talimatı (test).") != std::string::npos, "kurulumda araclar yok: %.400s", srv.lastSetup.c_str());
  run(800);
  size_t tx0 = srv.texts.size();
  const char *CALL = "{\"type\":\"response.event\",\"delegation_id\":\"d1\",\"event\":{\"type\":\"response.output_item.done\","
                     "\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"set_volume\",\"arguments\":\"{\\\"level\\\":30}\"}}}";
  srv.push(CALL);
  srv.push(CALL);                                       // aynı çağrı ikinci kez: bir kez yürütülür
  run(400);
  CHECK(toolRuns == 1 && lastToolName == "set_volume" && lastToolArgs == "{\"level\":30}", "arac calismadi (%d, %s %s)", toolRuns,
        lastToolName.c_str(), lastToolArgs.c_str());
  CHECK(textsWith("\"response.item.create\"", tx0) == 1 && textsWith("\"call_id\":\"call_1\"", tx0) == 1 &&
        textsWith("Ses %30 yapıldı.", tx0) == 1, "sonuc gitmedi");
  CHECK(textsWith("\"response.create\"", tx0) == 1, "devam mesaji gitmedi (%zu)", textsWith("\"response.create\"", tx0));
  {
    size_t a = 0, b = 0;
    for (size_t i = tx0; i < srv.texts.size(); i++) {
      if (srv.texts[i].find("response.item.create") != std::string::npos) a = i;
      if (srv.texts[i].find("\"response.create\"") != std::string::npos) b = i;
    }
    CHECK(b > a, "devam mesaji sonuctan once gitti");
  }
  CHECK(logHas("[araç] set_volume {\"level\":30} -> Ses %30 yapıldı."), "arac satiri yok");
  // Kullanıcının tam sözü kancaya gelir (yerel ses komutu yedeği için)
  srv.push("{\"type\":\"session.input_transcript.delta\",\"delta\":\"Sesi biraz kıs\"}");
  run(2000);
  CHECK(lastHeard == "Sesi biraz kıs", "duyulan soz kancaya gelmedi: '%s'", lastHeard.c_str());
  CHECK(lines.find("S:Sesi biraz kıs|") != std::string::npos, "satir kancasi: %s", lines.c_str());
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");

  printf("== 18) bildirim: uykudayken oturum açılır, not modele gider\n");
  run(WAKE_HOLDOFF_MS + 300);
  tx0 = srv.texts.size();
  int ch0 = chimes;
  linkNotify("Zamanlayıcı çaldı: çay. Kullanıcıya kısaca söyle.");
  CHECK(runUntil(LINK_LISTEN, 4000), "bildirimde baglanmadi (%s)", linkStateName());
  run(800);
  CHECK(chimes == ch0 + 1, "hazir sesi");
  CHECK(textsWith("\"session.instructions.append\"", tx0) == 1 && textsWith("Zamanlayıcı çaldı: çay.", tx0) == 1, "not gitmedi");
  CHECK(logHas("[bildirim] Zamanlayıcı çaldı: çay."), "bildirim satiri yok");
  // Oturum açıkken gelen bildirim hemen gider
  tx0 = srv.texts.size();
  linkNotify("Alarm çaldı: ilaç.");
  run(300);
  CHECK(textsWith("Alarm çaldı: ilaç.", tx0) == 1, "acik oturumda not gitmedi");
  int uc = usageCalls;
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  CHECK(usageCalls == uc + 1 && usageSpoke, "bildirim oturumu konusmasiz sayildi");

  printf("== 19) en büyük talimat (dolu hafıza) + araçlar + geçmiş kurulum mesajına sığar\n");
  run(WAKE_HOLDOFF_MS + 300);
  bigSys = true;
  linkWake();
  CHECK(runUntil(LINK_LISTEN, 4000), "buyuk talimatla baglanmadi (%s)", linkStateName());
  CHECK(srv.lastSetup.find("SON-ISARET") != std::string::npos && srv.lastSetup.find("\"name\":\"set_volume\"") != std::string::npos,
        "kurulum kesildi (%zu bayt)", srv.lastSetup.size());
  printf("   kurulum mesajı %zu bayt\n", srv.lastSetup.size());
  bigSys = false;
  linkSleep();
  CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");

  printf("== 20) cevabın ortasında ses geç gelince ön tampon oturum boyunca büyür, yeni oturumda sıfırlanır\n");
  {
    extern uint32_t fakeUnderruns;
    extern int fakePrebuf;
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    CHECK(fakePrebuf == GPTLIVE_PREBUF_MS, "baslangic on tampon %d", fakePrebuf);
    run(800);
    novaSpeaks(400);
    run(350);                                           // akış durdu: 350 ms hiç parça gelmedi
    fakeUnderruns++;                                    // hoparlör tamponu boşaldı
    novaSpeaks(300);                                    // ses geç geldi
    CHECK(fakePrebuf == GPTLIVE_PREBUF_MS + 150 && logHas("ön tampon 350 ms"), "on tampon buyumedi: %d", fakePrebuf);
    for (int k = 0; k < 4; k++) { run(300); fakeUnderruns++; novaSpeaks(200); }
    CHECK(fakePrebuf == GPTLIVE_PREBUF_MAX_MS, "ust sinir: %d", fakePrebuf);
    run(4000);
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    CHECK(fakePrebuf == GPTLIVE_PREBUF_MS, "yeni oturumda sifirlanmadi: %d", fakePrebuf);
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  }

  printf("== 20b) model cümlenin ortasında duraksarsa (akış sessizlikle sürer) ön tampon büyümez (2.8.1)\n");
  {
    extern uint32_t fakeUnderruns;
    extern int fakePrebuf;
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(800);
    novaSpeaks(400);
    for (int t = 0; t < 900; t += 100) {                // model 0,9 sn susuyor ama sessizlik parçaları geliyor
      srv.push("{\"type\":\"session.output_audio.delta\",\"delta\":\"" + silentAudio(100) + "\"}");
      run(100);
    }
    fakeUnderruns++;                                    // sessizlik çalınmadığı için hoparlör boşaldı
    novaSpeaks(300);
    CHECK(fakePrebuf == GPTLIVE_PREBUF_MS, "model duraksamasinda on tampon buyudu: %d", fakePrebuf);
    CHECK(logHas("cümlenin ortasında duraksadı") && !logHas("ses geç geldi (akışta 1"), "duraksama yazilmadi");
    run(4000);
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  }

  printf("== 20c) oturumda 2 sn'de bir ping gider, pong'la gidiş-dönüş süresi durum satırına yazılır\n");
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    int p0 = srv.pings;
    run(5000);
    CHECK(srv.pings - p0 >= 2 && srv.pings - p0 <= 3, "ping sayisi %d", srv.pings - p0);
    linkPrintStats();
    CHECK(logHas(" pong, 0 kayıp)") && !logHas("(0 pong"), "ping/pong durum satirinda yok");
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  }

  printf("== 21) uzun cevap \"görüşürüz\" ile bitiyorsa oturum kapanır; ortasında geçiyorsa kapanmaz\n");
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(1500);
    size_t v0 = logCount("Nova vedalaştı");
    liveTurn("Görüşmek için erken mi", "Görüşürüz demek için daha erken bence, anlat bakalım ne oldu?");
    run(4000);
    CHECK(linkState() != LINK_IDLE && logCount("Nova vedalaştı") == v0, "ortadaki gorusuruz kapatti (%s)", linkStateName());
    liveTurn("Sağ ol ya teşekkür ediyorum", "Ne demek, aklına gelmeyenler benim mesaim. İyi uykular, görüşürüz.");
    CHECK(runUntil(LINK_IDLE, 5000), "sonundaki gorusuruz kapatmadi (%s)", linkStateName());
    CHECK(logCount("Nova vedalaştı") == v0 + 1, "Nova veda satiri yok");
  }

  printf("== 22) cevap soruyla bitmedi ve %d ms konuşulmadı: oturum kapanır\n", LINK_SHORT_CLOSE_MS);
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(1500);
    liveTurn("Saat kaç", "Üçü çeyrek geçiyor.");
    CHECK(runUntil(LINK_LISTEN, 4000), "cevap bitmedi");
    uint32_t t0 = fakeNow;
    CHECK(runUntil(LINK_IDLE, LINK_SHORT_CLOSE_MS + 2000), "kisa kapanis olmadi (%s)", linkStateName());
    uint32_t sure = fakeNow - t0;
    CHECK(sure >= LINK_SHORT_CLOSE_MS - 100 && sure < LINK_SHORT_CLOSE_MS + 1500, "kisa kapanis suresi %u ms", (unsigned)sure);
    CHECK(logHas("cevap soruyla bitmedi"), "kisa kapanis satiri yok");
  }

  printf("== 23) cevap soruyla bittiyse kısa kapanış yok, normal uyku süresi geçerli\n");
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(1500);
    size_t k0 = logCount("cevap soruyla bitmedi");
    liveTurn("Yarın erken kalkacağım", "Alarm kurayım mı?");
    CHECK(runUntil(LINK_LISTEN, 4000), "cevap bitmedi");
    run(LINK_SHORT_CLOSE_MS + 3000);
    CHECK(linkState() == LINK_LISTEN, "soruda kapandi (%s)", linkStateName());
    CHECK(runUntil(LINK_IDLE, LINK_IDLE_SLEEP_MS + 2000), "normal uyku olmadi");
    CHECK(logCount("cevap soruyla bitmedi") == k0, "soruda kisa kapanis satiri");
  }

  printf("== 23b) cevap cümlenin ortasında kesildiyse (ses takıldı) kısa kapanış yok (2.8.1)\n");
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(1500);
    size_t k0 = logCount("cevap soruyla bitmedi");
    liveTurn("Hoca ne dedi biliyor musun", "Yani önce sana kendi bildiğinle bir yoğur diyor, kendi");
    CHECK(runUntil(LINK_LISTEN, 4000), "cevap bitmedi");
    run(LINK_SHORT_CLOSE_MS + 3000);
    CHECK(linkState() == LINK_LISTEN, "yarim cumlede kapandi (%s)", linkStateName());
    CHECK(logCount("cevap soruyla bitmedi") == k0, "yarim cumlede kisa kapanis satiri");
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  }

  printf("== 24) kısa kapanıştan önce kullanıcı konuşursa oturum sürer\n");
  {
    run(WAKE_HOLDOFF_MS + 300);
    linkWake();
    CHECK(runUntil(LINK_LISTEN, 4000), "baglanmadi");
    run(1500);
    liveTurn("Saat kaç", "Üçü çeyrek geçiyor.");
    CHECK(runUntil(LINK_LISTEN, 4000), "cevap bitmedi");
    run(LINK_SHORT_CLOSE_MS - 1500);
    srv.push("{\"type\":\"session.input_transcript.delta\",\"delta\":\"Bir de şunu\"}");
    run(LINK_SHORT_CLOSE_MS + 1000);
    CHECK(linkState() != LINK_IDLE, "kullanici konusurken kisa kapanis (%s)", linkStateName());
    linkSleep();
    CHECK(runUntil(LINK_IDLE, 3000), "uyumadi");
  }

  printf("\nSONUC: %s (%d hata)\n", fails ? "BASARISIZ" : "GECTI", fails);
  return fails ? 1 : 0;
}
