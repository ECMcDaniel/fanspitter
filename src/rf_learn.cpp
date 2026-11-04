#include "rf_learn.h"
#include "pulse_match.h"
#include <driver/gpio.h>
#include <esp_timer.h>   // esp_timer_get_time() — IRAM-safe µs clock

// ────────────────────────────────────────────────────────────────────────────
// ISR ring buffer (DRAM, shared between ISR and the loop task; both run on
// the same core because the ISR is attached from the loop task)
// ────────────────────────────────────────────────────────────────────────────
struct PulseRingBuffer {
  volatile uint16_t buf[RF_MAX_PULSES];
  volatile uint16_t head;       // ISR writes
  volatile uint16_t tail;       // loop reads
  volatile bool     frameReady; // ISR sets when an end-of-frame gap is seen
  volatile bool     overflow;   // ISR sets when the buffer is full
  volatile uint32_t lastEdgeUs; // timestamp of last edge (µs)
};

static PulseRingBuffer s_rb;

static void resetRingBuffer() {
  s_rb.head       = 0;
  s_rb.tail       = 0;
  s_rb.frameReady = false;
  s_rb.overflow   = false;
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();
}

// ────────────────────────────────────────────────────────────────────────────
// ISR — in IRAM, records the width of every level between edges on GDO0.
// ────────────────────────────────────────────────────────────────────────────
static void IRAM_ATTR gdo0IsrHandler() {
  uint32_t now   = (uint32_t)esp_timer_get_time();
  uint32_t width = now - s_rb.lastEdgeUs;
  s_rb.lastEdgeUs = now;

  if (width < RF_GLITCH_US) return;

  // A long quiet period ends a frame. The edge that ends the gap is the
  // start of the next frame's first (carrier-on) pulse.
  if (width >= RF_FRAME_GAP_US) {
    if (s_rb.head != s_rb.tail) s_rb.frameReady = true;
    return;
  }

  uint16_t next = (s_rb.head + 1) % RF_MAX_PULSES;
  if (next == s_rb.tail) {
    s_rb.overflow = true;
    return;
  }
  s_rb.buf[s_rb.head] = (uint16_t)width;  // < RF_FRAME_GAP_US, fits in 16 bits
  s_rb.head = next;
}

// ────────────────────────────────────────────────────────────────────────────
// Construction / setup
// ────────────────────────────────────────────────────────────────────────────
RFLearn::RFLearn(CC1101& cc1101, uint8_t gdo0Pin)
  : _cc(cc1101),
    _gdo0Pin(gdo0Pin),
    _state(State::IDLE),
    _captureCount(0),
    _confirmedLen(0),
    _confirmedPending(false),
    _learnStartMs(0) {
  memset(_captureLen, 0, sizeof(_captureLen));
}

void RFLearn::begin() {
  resetRingBuffer();
  _cc.setIdle();
  // Pull-down keeps GDO0 low whenever the CC1101 isn't driving it, so the
  // carrier stays off and the strapping pin reads low on reset.
  pinMode(_gdo0Pin, INPUT_PULLDOWN);
}

// ────────────────────────────────────────────────────────────────────────────
// process() — state machine tick; call every loop()
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::process() {
  if (_state != State::LEARNING && _state != State::CONFIRMING) return;

  if ((millis() - _learnStartMs) > RF_LEARN_TIMEOUT_MS) {
    Serial.println("[RF] Learn timeout");
    finishLearn(false, "timeout");
    return;
  }

  if (s_rb.overflow) {
    s_rb.overflow = false;
    Serial.println("[RF] WARN: ring buffer overflow (long signal or noise)");
  }

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

  if (confirmFrames()) {
    Serial.printf("[RF] Signal confirmed (%u pulses)\n", _confirmedLen);
    _confirmedPending = true;
    finishLearn(true, "confirmed");
  } else {
    // Keep the newer frame as the reference and wait for another press.
    Serial.println("[RF] Frames didn't match — waiting for next press");
    memcpy(_capture[0], _capture[1], _captureLen[1] * sizeof(uint16_t));
    _captureLen[0] = _captureLen[1];
    _captureCount  = 1;
  }
}

// ────────────────────────────────────────────────────────────────────────────
// Learn control
// ────────────────────────────────────────────────────────────────────────────
bool RFLearn::startLearn() {
  if (_state != State::IDLE) return false;
  _captureCount     = 0;
  _confirmedPending = false;
  _learnStartMs     = millis();
  _state            = State::LEARNING;

  resetRingBuffer();
  _cc.setIdle();
  _cc.enterRX();
  attachISR();
  Serial.println("[RF] Learn mode started — press remote button");
  return true;
}

void RFLearn::cancelLearn() {
  if (_state != State::LEARNING && _state != State::CONFIRMING) return;
  stopRx();
  _captureCount = 0;
  _state = State::IDLE;
  Serial.println("[RF] Learn cancelled");
}

void RFLearn::finishLearn(bool ok, const char* reason) {
  stopRx();
  _captureCount = 0;
  _state = State::IDLE;
  if (_learnDoneCb) _learnDoneCb(ok, reason);
}

void RFLearn::stopRx() {
  detachISR();
  _cc.setIdle();
  pinMode(_gdo0Pin, INPUT_PULLDOWN);
}

uint16_t RFLearn::getConfirmedPulses(uint16_t* outBuf, uint16_t maxLen) const {
  if (!_confirmedPending || _confirmedLen == 0) return 0;
  uint16_t n = (_confirmedLen < maxLen) ? _confirmedLen : maxLen;
  memcpy(outBuf, _confirmedBuf, n * sizeof(uint16_t));
  return n;
}

// ────────────────────────────────────────────────────────────────────────────
// Copy pulses up to the current head into _capture[_captureCount].
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::drainFrameFromRingBuffer() {
  uint16_t dst = 0;
  uint16_t h   = s_rb.head;  // snapshot; ISR may keep appending

  while (s_rb.tail != h && dst < RF_MAX_PULSES) {
    _capture[_captureCount][dst++] = s_rb.buf[s_rb.tail];
    s_rb.tail = (s_rb.tail + 1) % RF_MAX_PULSES;
  }
  _captureLen[_captureCount] = dst;
}

bool RFLearn::confirmFrames() {
  const PulseMatchParams p = {
    RF_MIN_PULSES, RF_CONFIRM_MAX_LEN_DIFF, RF_CONFIRM_TOL_PCT, RF_CONFIRM_TOL_MIN_US
  };
  uint16_t at = 0;
  uint16_t n = matchFrames(_capture[0], _captureLen[0],
                           _capture[1], _captureLen[1],
                           p, _confirmedBuf, &at);
  if (n == 0) {
    if (at == 0xFFFF) {
      Serial.printf("[RF] Length mismatch: %u vs %u\n", _captureLen[0], _captureLen[1]);
    } else {
      Serial.printf("[RF] Pulse %u mismatch: %u vs %u\n",
                    at, _capture[0][at], _capture[1][at]);
    }
    return false;
  }
  _confirmedLen = n;
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// transmit() — bit-bang the pulse train on GDO0 with the CC1101 in async TX.
// ────────────────────────────────────────────────────────────────────────────
bool RFLearn::transmit(const uint16_t* pulses, uint16_t count) {
  if (_state != State::IDLE || !pulses || count == 0) return false;
  _state = State::TRANSMITTING;

  _cc.setIdle();
  if (!_cc.enterTX()) {
    Serial.printf("[RF] ERROR: CC1101 did not enter TX (MARCSTATE=0x%02X)\n",
                  _cc.getMarcState());
    _cc.setIdle();
    _state = State::IDLE;
    return false;
  }

  const gpio_num_t pin = (gpio_num_t)_gdo0Pin;
  pinMode(_gdo0Pin, OUTPUT);
  gpio_set_level(pin, 0);
  delayMicroseconds(500);  // let the PA settle with the carrier off

  // Mask interrupts on this core during each frame for clean timing, unless
  // the frame is long enough to risk the interrupt watchdog.
  const bool mask = frameDurationUs(pulses, count) <= RF_TX_MAX_MASKED_US;
  if (!mask) Serial.println("[RF] WARN: long frame — sending without masking interrupts");

  for (uint8_t rep = 0; rep < RF_TX_REPEATS; rep++) {
    uint32_t level = 1;  // first captured pulse is carrier-on
    if (mask) portDISABLE_INTERRUPTS();
    for (uint16_t i = 0; i < count; i++) {
      gpio_set_level(pin, level);
      delayMicroseconds(pulses[i]);
      level ^= 1;
    }
    gpio_set_level(pin, 0);
    if (mask) portENABLE_INTERRUPTS();

    if (rep + 1 < RF_TX_REPEATS) delay(RF_TX_GAP_MS);
  }

  _cc.setIdle();
  pinMode(_gdo0Pin, INPUT_PULLDOWN);
  _state = State::IDLE;
  Serial.printf("[RF] Transmitted %u pulses x %u\n", count, RF_TX_REPEATS);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// ISR attach / detach
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::attachISR() {
  pinMode(_gdo0Pin, INPUT);
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(_gdo0Pin), gdo0IsrHandler, CHANGE);
}

void RFLearn::detachISR() {
  detachInterrupt(digitalPinToInterrupt(_gdo0Pin));
}
