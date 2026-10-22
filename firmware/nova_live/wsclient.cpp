#include "wsclient.h"
#include "b64.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void WsClient::begin(WsTransport *t, uint8_t *rxBuf, size_t rxCap, uint8_t *txBuf, size_t txCap,
                     uint32_t (*nowMs)(), uint32_t (*rnd)(), void (*idle)()) {
  t_ = t;
  rx_ = rxBuf; rxCap_ = rxCap;
  tx_ = txBuf; txCap_ = txCap;
  now_ = nowMs; rnd_ = rnd; idle_ = idle;
  open_ = false;
  resetRx();
}

void WsClient::resetRx() {
  state_ = S_HDR;
  hdrHave_ = 0; hdrNeed_ = 2;
  payLen_ = payRead_ = 0;
  msgLen_ = 0; ctrlLen_ = 0;
  inMsg_ = msgReady_ = truncated_ = false;
  pingSentAt_ = 0;
}

void WsClient::fail(const char *why) {
  strncpy(error_, why, sizeof(error_) - 1);
  error_[sizeof(error_) - 1] = 0;
}

bool WsClient::connect(const char *host, uint16_t port, const char *path, const char *extraHeaders, uint32_t timeoutMs) {
  open_ = false;
  closeCode_ = 0; closeReason_[0] = 0; error_[0] = 0;
  httpStatus_ = 0; httpBody_[0] = 0;
  resetRx();
  uint32_t t0 = now_();

  if (!t_->open(host, port, timeoutMs)) { fail("sunucuya bağlanılamadı (TCP/TLS)"); return false; }

  uint8_t kb[16];
  for (int i = 0; i < 16; i += 4) { uint32_t r = rnd_(); memcpy(kb + i, &r, 4); }
  char key[28];
  key[b64Encode(kb, 16, key)] = 0;

  int n = snprintf((char *)tx_, txCap_,
                   "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: nova-esp32\r\n%s\r\n",
                   path, host, key, extraHeaders ? extraHeaders : "");
  if (n <= 0 || (size_t)n >= txCap_) { t_->close(); fail("istek tampona sığmadı"); return false; }
  if (!t_->write(tx_, (size_t)n)) { t_->close(); fail("istek gönderilemedi"); return false; }

  // Yanıt başlığını satır sonu çiftine kadar oku (fazlasını okuma: ardından çerçeveler gelir).
  // Başlık istenen kadar uzun olabilir (çerezler vb.); yalnızca ilk kısmı saklanır,
  // bitişi son dört bayta bakarak bulunur.
  char hb[1024];
  size_t hl = 0;
  uint32_t last4 = 0;
  bool done = false;
  while (!done && now_() - t0 < timeoutMs) {
    if (t_->available() <= 0) {
      if (!t_->connected()) break;
      if (idle_) idle_();
      continue;
    }
    uint8_t c;
    if (t_->read(&c, 1) != 1) continue;
    if (hl < sizeof(hb) - 1) hb[hl++] = (char)c;
    last4 = (last4 << 8) | c;
    if (last4 == 0x0D0A0D0Au) done = true;
  }
  hb[hl] = 0;
  if (!done) { t_->close(); fail("sunucu yükseltme yanıtını göndermedi"); return false; }

  const char *sp = strchr(hb, ' ');
  httpStatus_ = sp ? atoi(sp + 1) : 0;
  if (httpStatus_ != 101) {
    // Hata gövdesini tanı için al (API anahtarı hataları burada yazar)
    size_t bl = 0;
    uint32_t last = now_();
    while (bl < sizeof(httpBody_) - 1 && now_() - last < 400) {
      if (t_->available() > 0) {
        int r = t_->read((uint8_t *)httpBody_ + bl, sizeof(httpBody_) - 1 - bl);
        if (r > 0) { bl += (size_t)r; last = now_(); }
      } else if (!t_->connected()) {
        break;
      } else if (idle_) {
        idle_();
      }
    }
    httpBody_[bl] = 0;
    t_->close();
    char *eol = strstr(hb, "\r\n");
    if (eol) *eol = 0;
    snprintf(error_, sizeof(error_), "sunucu bağlantıyı reddetti: %.100s", hb);
    return false;
  }
  open_ = true;
  return true;
}

bool WsClient::send(uint8_t opcode, size_t len) {
  if (!open_ || len > txPayloadCap()) return false;
  uint8_t *p = tx_ + kTxHead;
  uint8_t h[kTxHead];
  size_t hl = 0;
  h[hl++] = 0x80 | opcode;
  if (len < 126) {
    h[hl++] = 0x80 | (uint8_t)len;
  } else if (len <= 0xFFFF) {
    h[hl++] = 0x80 | 126;
    h[hl++] = (uint8_t)(len >> 8);
    h[hl++] = (uint8_t)len;
  } else {
    h[hl++] = 0x80 | 127;
    uint64_t l = len;
    for (int i = 7; i >= 0; i--) h[hl++] = (uint8_t)(l >> (8 * i));
  }
  uint32_t m = rnd_();
  uint8_t mk[4];
  memcpy(mk, &m, 4);
  memcpy(h + hl, mk, 4);
  hl += 4;
  for (size_t i = 0; i < len; i++) p[i] ^= mk[i & 3];
  memcpy(p - hl, h, hl);
  if (!t_->write(p - hl, hl + len)) {
    fail("yazma hatası");
    open_ = false;
    t_->close();
    return false;
  }
  txMsgs++;
  return true;
}

bool WsClient::sendControl(uint8_t opcode, const uint8_t *data, size_t len) {
  if (!open_) return false;
  if (len > 125) len = 125;
  uint8_t f[2 + 4 + 125];
  uint32_t m = rnd_();
  uint8_t mk[4];
  memcpy(mk, &m, 4);
  f[0] = 0x80 | opcode;
  f[1] = 0x80 | (uint8_t)len;
  memcpy(f + 2, mk, 4);
  for (size_t i = 0; i < len; i++) f[6 + i] = data[i] ^ mk[i & 3];
  return t_->write(f, 6 + len);
}

bool WsClient::ping() {
  uint32_t n = now_ ? now_() : 0;
  if (pingSentAt_) {
    if ((int32_t)(n - pingSentAt_) < 10000) return true;   // önceki ping hâlâ bekliyor
    pingsLost++;
  }
  pingSentAt_ = n ? n : 1;
  return sendControl(OP_PING, nullptr, 0);
}

void WsClient::close(uint16_t code) {
  if (open_) {
    uint8_t c[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    sendControl(OP_CLOSE, c, 2);
  }
  open_ = false;
  if (t_) t_->close();
  resetRx();
}

void WsClient::consume() {
  msgReady_ = false;
  msgLen_ = 0;
  truncated_ = false;
}

void WsClient::frameDone() {
  state_ = S_HDR;
  hdrHave_ = 0;
  hdrNeed_ = 2;
  if (isCtrl_) {
    if (opcode_ == OP_PING) {
      sendControl(OP_PONG, ctrl_, ctrlLen_);
    } else if (opcode_ == OP_PONG) {
      if (pingSentAt_ && now_) {
        uint32_t r = now_() - pingSentAt_;
        rttLast = r;
        if (r > rttMax) rttMax = r;
        pongs++;
      }
      pingSentAt_ = 0;
    } else if (opcode_ == OP_CLOSE) {
      closeCode_ = ctrlLen_ >= 2 ? (uint16_t)((ctrl_[0] << 8) | ctrl_[1]) : 1005;
      size_t n = ctrlLen_ > 2 ? ctrlLen_ - 2 : 0;
      if (n > sizeof(closeReason_) - 1) n = sizeof(closeReason_) - 1;
      memcpy(closeReason_, ctrl_ + 2, n);
      closeReason_[n] = 0;
      sendControl(OP_CLOSE, ctrl_, ctrlLen_ >= 2 ? 2 : 0);
      fail("sunucu bağlantıyı kapattı");
      open_ = false;
      t_->close();
    }
    return;
  }
  if (fin_) {
    inMsg_ = false;
    msgReady_ = true;
    rx_[msgLen_] = 0;
    rxMsgs++;
    if (msgLen_ > maxMsg) maxMsg = msgLen_;
  }
}

int WsClient::poll() {
  if (msgReady_) return 1;
  if (!open_) return -1;
  for (;;) {
    int avail = t_->available();
    if (avail <= 0) {
      if (!t_->connected()) {
        fail("bağlantı koptu");
        open_ = false;
        t_->close();
        return -1;
      }
      return 0;
    }

    if (state_ != S_PAYLOAD) {
      size_t want = hdrNeed_ - hdrHave_;
      if (want > (size_t)avail) want = (size_t)avail;
      int r = t_->read(hdr_ + hdrHave_, want);
      if (r <= 0) return 0;
      rxBytes += (uint32_t)r;
      hdrHave_ += (size_t)r;
      if (hdrHave_ < hdrNeed_) continue;

      if (state_ == S_HDR) {
        fin_ = (hdr_[0] & 0x80) != 0;
        opcode_ = hdr_[0] & 0x0F;
        masked_ = (hdr_[1] & 0x80) != 0;
        isCtrl_ = opcode_ >= 8;
        uint8_t l7 = hdr_[1] & 0x7F;
        if (l7 == 126) { hdrNeed_ = 4; state_ = S_EXT; continue; }
        if (l7 == 127) { hdrNeed_ = 10; state_ = S_EXT; continue; }
        payLen_ = l7;
      } else if (state_ == S_EXT) {
        if (hdrNeed_ == 4) {
          payLen_ = ((uint64_t)hdr_[2] << 8) | hdr_[3];
        } else {
          payLen_ = 0;
          for (int i = 2; i < 10; i++) payLen_ = (payLen_ << 8) | hdr_[i];
        }
      }
      if (state_ != S_MASK && masked_) { hdrNeed_ = hdrHave_ + 4; state_ = S_MASK; continue; }
      if (state_ == S_MASK) memcpy(mask_, hdr_ + hdrHave_ - 4, 4);

      payRead_ = 0;
      if (isCtrl_) {
        ctrlLen_ = 0;
      } else if (opcode_ != OP_CONT || !inMsg_) {
        msgLen_ = 0;
        truncated_ = false;
        inMsg_ = true;
      }
      state_ = S_PAYLOAD;
      if (payLen_ == 0) {
        frameDone();
        if (msgReady_) return 1;
        if (!open_) return -1;
      }
      continue;
    }

    uint64_t left = payLen_ - payRead_;
    size_t chunk = left < (uint64_t)avail ? (size_t)left : (size_t)avail;
    size_t room = isCtrl_ ? sizeof(ctrl_) - ctrlLen_ : rxCap_ - 1 - msgLen_;
    int r;
    if (room == 0) {
      // Sığmayan kısım okunup atılır; mesaj "kesik" diye işaretlenir.
      uint8_t scratch[256];
      if (chunk > sizeof(scratch)) chunk = sizeof(scratch);
      r = t_->read(scratch, chunk);
      if (r <= 0) return 0;
      if (!isCtrl_) truncated_ = true;
    } else {
      if (chunk > room) chunk = room;
      uint8_t *dst = isCtrl_ ? ctrl_ + ctrlLen_ : rx_ + msgLen_;
      r = t_->read(dst, chunk);
      if (r <= 0) return 0;
      if (masked_) for (int i = 0; i < r; i++) dst[i] ^= mask_[(payRead_ + (uint64_t)i) & 3];
      if (isCtrl_) ctrlLen_ += (size_t)r; else msgLen_ += (size_t)r;
    }
    rxBytes += (uint32_t)r;
    payRead_ += (uint64_t)r;
    if (payRead_ == payLen_) {
      frameDone();
      if (msgReady_) return 1;
      if (!open_) return -1;
    }
  }
}
