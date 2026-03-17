#include "rf_learn.h"
#include <esp_timer.h>   // esp_timer_get_time() — IRAM-safe µs clock

// ────────────────────────────────────────────────────────────────────────────
// Singleton instance (used by global ISR)
// ────────────────────────────────────────────────────────────────────────────
RFLearn* RFLearn::instance = nullptr;

// Global ring-buffer in DRAM (reachable from IRAM ISR)
static PulseRingBuffer s_rb;

// ────────────────────────────────────────────────────────────────────────────
// ISR — must be in IRAM, must be fast
// ────────────────────────────────────────────────────────────────────────────
void IRAM_ATTR gdo0IsrHandler() {
  uint32_t now = (uint32_t)esp_timer_get_time();  // µs, 32-bit is fine (wraps ~71 min)
  uint32_t width = now - s_rb.lastEdgeUs;
  s_rb.lastEdgeUs = now;

  // Glitch filter: ignore edges < 50 µs
  if (width < 50) return;

  // End-of-frame gap detection
  if (width >= RF_FRAME_GAP_US) {
    // Signal frame just ended — flag it if we captured something
    if (s_rb.head != s_rb.tail) {
      s_rb.frameReady = true;
    }
    return;
  }

  // Store pulse width in ring buffer
  uint16_t next = (s_rb.head + 1) % RF_MAX_PULSES;
  if (next == s_rb.tail) {
    s_rb.overflow = true;
    return;  // discard; don't move head
  }
  // Clamp to uint16_t max (65535 µs ≈ 65 ms — far beyond any fan pulse)
  s_rb.buf[s_rb.head] = (uint16_t)(width > 65535 ? 65535 : width);
  s_rb.head = next;
}

// ────────────────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────────────────
RFLearn::RFLearn(CC1101& cc1101, uint8_t gdo0Pin)
  : _cc(cc1101),
    _gdo0Pin(gdo0Pin),
    _state(State::IDLE),
    _captureCount(0),
    _confirmedLen(0),
    _confirmedPending(false),
    _txPulses(nullptr),
    _txCount(0),
    _learnStartMs(0)
{
  memset(_captureLen, 0, sizeof(_captureLen));
  instance = this;
}

// ────────────────────────────────────────────────────────────────────────────
// begin() — attach ISR, put CC1101 in RX
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::begin() {
  // Initialise ring buffer
  s_rb.head       = 0;
  s_rb.tail       = 0;
  s_rb.frameReady = false;
  s_rb.overflow   = false;
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();

  _cc.enterRX();
  pinMode(_gdo0Pin, INPUT);
  attachISR();
}

// ────────────────────────────────────────────────────────────────────────────
// process() — state machine tick; call every loop()
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::process() {
  switch (_state) {

  case State::LEARNING:
  case State::CONFIRMING: {
    // Learn timeout watchdog
    if ((millis() - _learnStartMs) > RF_LEARN_TIMEOUT_MS) {
      Serial.println("[RF] Learn timeout");
      _state = State::IDLE;
      _captureCount = 0;
      if (_learnDoneCb) _learnDoneCb(false, "timeout");
      return;
    }

    // Log overflow but don't abort — signal may still be usable
    if (s_rb.overflow) {
      s_rb.overflow = false;
      Serial.println("[RF] WARN: ring buffer overflow (long signal?)");
    }

    // Frame available?
    if (!s_rb.frameReady) return;
    s_rb.frameReady = false;

    drainFrameFromRingBuffer();

    if (_captureLen[_captureCount] < RF_MIN_PULSES) {
      Serial.printf("[RF] Frame too short (%u pulses), ignoring\n",
                    _captureLen[_captureCount]);
      return;
    }

    _captureCount++;

    if (_captureCount == 1) {
      Serial.printf("[RF] Frame 1 captured (%u pulses) — press remote again\n",
                    _captureLen[0]);
      _state = State::CONFIRMING;
      return;
    }

    // We have two frames — try to confirm
    if (confirmFrames()) {
      Serial.printf("[RF] Signal confirmed! (%u pulses stored)\n", _confirmedLen);
      _confirmedPending = true;
      _state = State::IDLE;
      _captureCount = 0;
      if (_learnDoneCb) _learnDoneCb(true, "confirmed");
    } else {
      // Shift: use second frame as first, wait for another
      Serial.println("[RF] Frames didn't match — waiting for next press");
      memcpy(_capture[0], _capture[1], _captureLen[1] * sizeof(uint16_t));
      _captureLen[0] = _captureLen[1];
      _captureCount = 1;
      // Stay in CONFIRMING
    }
    break;
  }

  case State::TRANSMITTING:
    // doTransmit() is synchronous; this case is never reached in normal flow.
    break;

  default:
    break;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// startLearn()
// ────────────────────────────────────────────────────────────────────────────
bool RFLearn::startLearn() {
  if (_state != State::IDLE) return false;
  _captureCount     = 0;
  _confirmedPending = false;
  _learnStartMs     = millis();
  _state            = State::LEARNING;
  // Clear ring buffer
  s_rb.head       = 0;
  s_rb.tail       = 0;
  s_rb.frameReady = false;
  s_rb.overflow   = false;
  Serial.println("[RF] Learn mode started — press remote button");
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// startTransmit()
// ────────────────────────────────────────────────────────────────────────────
bool RFLearn::startTransmit(const uint16_t* pulses, uint16_t count) {
  if (_state != State::IDLE) return false;
  if (!pulses || count == 0) return false;
  _txPulses = pulses;
  _txCount  = count;
  _state    = State::TRANSMITTING;
  doTransmit();  // synchronous; sets _state = IDLE when done
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// getConfirmedPulses()
// ────────────────────────────────────────────────────────────────────────────
uint16_t RFLearn::getConfirmedPulses(uint16_t* outBuf, uint16_t maxLen) const {
  if (!_confirmedPending || _confirmedLen == 0) return 0;
  uint16_t n = (_confirmedLen < maxLen) ? _confirmedLen : maxLen;
  memcpy(outBuf, _confirmedBuf, n * sizeof(uint16_t));
  return n;
}

// ────────────────────────────────────────────────────────────────────────────
// drainFrameFromRingBuffer()
// Copies pending pulse widths from ring buffer into _capture[_captureCount].
// Must only be called from the main task, not from ISR.
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::drainFrameFromRingBuffer() {
  uint16_t dst = 0;
  uint16_t h   = s_rb.head;         // snapshot (ISR may still advance after this)

  while (s_rb.tail != h && dst < RF_MAX_PULSES) {
    _capture[_captureCount][dst++] = s_rb.buf[s_rb.tail];
    s_rb.tail = (s_rb.tail + 1) % RF_MAX_PULSES;
  }
  _captureLen[_captureCount] = dst;
}

// ────────────────────────────────────────────────────────────────────────────
// confirmFrames()
// Compare _capture[0] vs _capture[1] pulse-by-pulse.
// Returns true if they match within RF_CONFIRM_TOL_PCT tolerance.
// On success, fills _confirmedBuf with the element-wise average.
// ────────────────────────────────────────────────────────────────────────────
bool RFLearn::confirmFrames() {
  uint16_t len0 = _captureLen[0];
  uint16_t len1 = _captureLen[1];

  if (len0 < RF_MIN_PULSES || len1 < RF_MIN_PULSES) return false;

  // Allow up to 2 pulses difference in length (sometimes preamble varies)
  if (len0 > len1 + 2 || len1 > len0 + 2) {
    Serial.printf("[RF] Length mismatch: %u vs %u\n", len0, len1);
    return false;
  }

  uint16_t len = (len0 < len1) ? len0 : len1;

  for (uint16_t i = 0; i < len; i++) {
    uint16_t a = _capture[0][i];
    uint16_t b = _capture[1][i];
    uint16_t ref  = (a > b) ? a : b;   // use the larger as reference
    uint16_t diff = (a > b) ? (a - b) : (b - a);
    if (diff > (uint32_t)ref * RF_CONFIRM_TOL_PCT / 100) {
      Serial.printf("[RF] Pulse %u mismatch: %u vs %u\n", i, a, b);
      return false;
    }
  }

  // Average the two captures for storage (reduces noise)
  for (uint16_t i = 0; i < len; i++) {
    _confirmedBuf[i] = (_capture[0][i] + _capture[1][i]) / 2;
  }
  _confirmedLen = len;
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// doTransmit()
// Bit-bangs the stored pulse train via GDO0.
// Called synchronously from startTransmit(); runs entirely in the loop task.
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::doTransmit() {
  // Step 1: detach ISR so we hold exclusive control of GDO0
  detachISR();

  // Step 2: switch CC1101 to TX (sets IOCFG0=0x0B)
  _cc.enterTX();

  // Step 3: MCU takes over GDO0 as digital output
  pinMode(_gdo0Pin, OUTPUT);
  digitalWrite(_gdo0Pin, LOW);
  delayMicroseconds(500);  // brief idle before first frame

  // Step 4: transmit RF_TX_REPEATS times with 20 ms inter-frame gaps
  for (uint8_t rep = 0; rep < RF_TX_REPEATS; rep++) {
    // Most OOK fan remotes: first pulse HIGH (carrier on), alternating after.
    bool level = HIGH;

    // Disable ISRs for timing accuracy during the bit-bang loop.
    // Total TX is typically 50–120 ms; one missed WiFi beacon is acceptable.
    portDISABLE_INTERRUPTS();
    for (uint16_t i = 0; i < _txCount; i++) {
      digitalWrite(_gdo0Pin, level);
      delayMicroseconds(_txPulses[i]);
      level = !level;
    }
    digitalWrite(_gdo0Pin, LOW);  // carrier off at frame end
    portENABLE_INTERRUPTS();

    if (rep < RF_TX_REPEATS - 1) {
      delay(20);  // 20 ms inter-frame gap
    }
  }

  // Step 5: return CC1101 to RX, re-attach ISR
  _cc.setIdle();
  _cc.enterRX();
  pinMode(_gdo0Pin, INPUT);
  attachISR();

  _state = State::IDLE;
  Serial.printf("[RF] Transmitted %u pulses × %u\n", _txCount, RF_TX_REPEATS);
  if (_txDoneCb) _txDoneCb(true, "ok");
}

// ────────────────────────────────────────────────────────────────────────────
// ISR attach / detach helpers
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::attachISR() {
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(_gdo0Pin),
                  gdo0IsrHandler, CHANGE);
}

void RFLearn::detachISR() {
  detachInterrupt(digitalPinToInterrupt(_gdo0Pin));
}
