// Küçük bir WebSocket istemcisi (RFC 6455). Donanıma bağlı değil: bağlantı
// WsTransport üzerinden yapılır (kartta TLS, masaüstü testinde düz TCP).
//
// Hazır kütüphane yerine bunun yazılma sebebi: Gemini Live tek mesajda onlarca
// KB yolluyor ve sunucular mesajları parçalı çerçevelerle gönderebiliyor. Burada mesaj,
// boyutu bizim belirlediğimiz (PSRAM'deki) tek bir tamponda birleştirilir.
#pragma once
#include <stdint.h>
#include <stddef.h>

struct WsTransport {
  virtual bool open(const char *host, uint16_t port, uint32_t timeoutMs) = 0;
  virtual void close() = 0;
  virtual bool connected() = 0;
  virtual int  available() = 0;                        // okunmaya hazır bayt
  virtual int  read(uint8_t *buf, size_t n) = 0;       // okunan bayt (yoksa 0)
  virtual bool write(const uint8_t *buf, size_t n) = 0; // hepsi yazıldıysa true
  virtual ~WsTransport() {}
};

class WsClient {
 public:
  enum : uint8_t { OP_CONT = 0, OP_TEXT = 1, OP_BIN = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };
  static const size_t kTxHead = 14;                    // en uzun çerçeve başlığı

  // idle: beklerken çağrılır (kartta 1 ms uyur), işlemciyi boşuna döndürmemek için.
  void begin(WsTransport *t, uint8_t *rxBuf, size_t rxCap, uint8_t *txBuf, size_t txCap,
             uint32_t (*nowMs)(), uint32_t (*rnd)(), void (*idle)());

  // Bağlanır ve HTTP yükseltmesini yapar. Olmazsa error() / httpStatus() / httpBody() dolar.
  // extraHeaders: isteğe eklenecek başlıklar, her biri "\r\n" ile biter (yoksa nullptr).
  bool connect(const char *host, uint16_t port, const char *path, const char *extraHeaders, uint32_t timeoutMs);
  void close(uint16_t code = 1000);
  bool isOpen() const { return open_; }

  // Gönderme: yük txPayload()'a yazılır, sonra send() çağrılır.
  uint8_t *txPayload() { return tx_ + kTxHead; }
  size_t txPayloadCap() const { return txCap_ - kTxHead; }
  bool send(uint8_t opcode, size_t len);
  // Ping yollar ve pong gelince gidiş-dönüş süresini (rtt) ölçer. Önceki ping 10 sn'dir cevapsızsa kayıp sayılır.
  bool ping();
  bool pingPending() const { return pingSentAt_ != 0; }

  // Alma: 1 = tam bir mesaj hazır (msg/msgLen), 0 = henüz yok, -1 = kapandı.
  int poll();
  uint8_t *msg() { return rx_; }
  size_t msgLen() const { return msgLen_; }
  bool msgTruncated() const { return truncated_; }
  void consume();

  uint16_t closeCode() const { return closeCode_; }
  const char *closeReason() const { return closeReason_; }
  const char *error() const { return error_; }
  int httpStatus() const { return httpStatus_; }
  const char *httpBody() const { return httpBody_; }

  uint32_t rxBytes = 0, rxMsgs = 0, txMsgs = 0;
  size_t maxMsg = 0;
  uint32_t rttLast = 0, rttMax = 0, pongs = 0, pingsLost = 0;   // ms; rttMax'ı okuyan sıfırlar

 private:
  enum : uint8_t { S_HDR, S_EXT, S_MASK, S_PAYLOAD };
  void resetRx();
  void fail(const char *why);
  bool sendControl(uint8_t opcode, const uint8_t *data, size_t len);
  void frameDone();

  uint32_t pingSentAt_ = 0;
  WsTransport *t_ = nullptr;
  uint8_t *rx_ = nullptr, *tx_ = nullptr;
  size_t rxCap_ = 0, txCap_ = 0;
  uint32_t (*now_)() = nullptr;
  uint32_t (*rnd_)() = nullptr;
  void (*idle_)() = nullptr;
  bool open_ = false;

  uint8_t state_ = S_HDR;
  uint8_t hdr_[14];
  size_t hdrHave_ = 0, hdrNeed_ = 2;
  bool fin_ = false, masked_ = false, isCtrl_ = false;
  uint8_t opcode_ = 0, mask_[4] = {0, 0, 0, 0};
  uint64_t payLen_ = 0, payRead_ = 0;
  size_t msgLen_ = 0;
  bool inMsg_ = false, msgReady_ = false, truncated_ = false;
  uint8_t ctrl_[128];
  size_t ctrlLen_ = 0;

  uint16_t closeCode_ = 0;
  char closeReason_[160] = "";
  char error_[160] = "";
  int httpStatus_ = 0;
  char httpBody_[400] = "";
};
