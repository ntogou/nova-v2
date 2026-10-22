// Ses ve ekran işlevlerinin sahteleri: link.cpp'nin kullandıkları
#include "audio.h"
#include "ui.h"
SerialMock Serial;
extern uint32_t fakeNow;
uint32_t micNextAt = 0;
int micFrames = 0;
bool micReadFrame(uint8_t *out) {
  if (fakeNow < micNextAt) return false;
  micNextAt = (micNextAt ? micNextAt : fakeNow) + 80;
  int16_t *s = (int16_t *)out;                    // sabit, sıfır olmayan ses: sessizlik çerçevesinden ayırt edilsin
  for (int i = 0; i < MIC_FRAME_SAMPLES; i++) s[i] = (int16_t)(i & 1 ? 300 : -300);
  micFrames++;
  return true;
}
size_t spkBytes = 0;
size_t spkWrite(const uint8_t *, size_t len) { spkBytes += len; return len; }
int spkFlushes = 0;
void spkFlush() { spkFlushes++; }
void spkMarkEnd() {}
uint32_t spkBufferedMs() { return 0; }
size_t spkFree() { return 1 << 20; }
bool audioTakePlayStart() { return false; }
bool audioTakePlayEnd() { return false; }
uint32_t fakeUnderruns = 0;
int fakePrebuf = 0;
uint32_t spkUnderrunCount() { return fakeUnderruns; }
void spkSetPrebufMs(int ms) { fakePrebuf = ms; }
void spkSetBoostDb(float) {}
uint16_t micLevel() { return 0; }
uint16_t spkLevel() { return 0; }
uint32_t spkOverflowCount() { return 0; }
void uiSetState(UiState) {}
void uiText(const char *, const char *, bool) {}
void uiClearText() {}
