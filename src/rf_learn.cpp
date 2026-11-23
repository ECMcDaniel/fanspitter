#include "rf_learn.h"
#include "pulse_match.h"
#include <driver/gpio.h>
#include <esp_timer.h>   // esp_timer_get_time() — IRAM-safe µs clock

// ────────────────────────────────────────────────────────────────────────────
// ISR frame buffer (DRAM, shared between ISR and the loop task; both run on
// the same core because the ISR is attached from the loop task).
//
// The ISR records exactly one frame at a time, from the edge that ends one
// carrier-off gap to the edge that ends the next, then parks until the loop
// task has collected it. Recording only gap-to-gap means every captured frame
// is whole, however long the loop task takes to get round to it, and a remote
// that repeats while the button is held gives two matching frames in one press.
// ────────────────────────────────────────────────────────────────────────────
struct PulseFrameBuffer {
  volatile uint16_t buf[RF_MAX_PULSES];
  volatile uint16_t count;      // entries recorded in the current frame
  volatile bool     inFrame;    // a gap has been seen; recording
  volatile bool     frameReady; // frame complete; ISR parked until collected
  volatile bool     overflow;   // a frame was dropped for being too long
  volatile uint32_t lastEdgeUs; // timestamp of last edge (µs)
};

static PulseFrameBuffer s_rb;
static gpio_num_t       s_gdo0 = GPIO_NUM_NC;

static void resetRingBuffer() {
  s_rb.count      = 0;
  s_rb.inFrame    = false;
  s_rb.overflow   = false;
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();
  s_rb.frameReady = false;      // last: this is what un-parks the ISR
}

// ────────────────────────────────────────────────────────────────────────────
// ISR — in IRAM, records the width of every level between edges on GDO0.
// ────────────────────────────────────────────────────────────────────────────
static void IRAM_ATTR gdo0IsrHandler() {
  uint32_t now   = (uint32_t)esp_timer_get_time();
  uint32_t width = now - s_rb.lastEdgeUs;
  s_rb.lastEdgeUs = now;

  if (s_rb.frameReady) return;           // previous frame not collected yet
  if (width < RF_GLITCH_US) return;

  // A rising edge after a long quiet (carrier-off) period starts a frame, and
  // ends the one before it. A long carrier-ON period is just a long pulse.
  const bool rising = gpio_get_level(s_gdo0) == 1;
  if (width >= RF_FRAME_GAP_US && rising) {
    if (s_rb.inFrame && s_rb.count > 0) {
      // Close the frame with its gap so a replay keeps the repeat spacing.
      s_rb.buf[s_rb.count] = (width > RF_GAP_STORE_MAX_US) ? RF_GAP_STORE_MAX_US : (uint16_t)width;
      s_rb.count = s_rb.count + 1;
      s_rb.frameReady = true;
    } else {
      s_rb.inFrame = true;               // first frame since learning started
    }
    return;
  }

  if (!s_rb.inFrame) return;             // joined mid-frame; wait for a gap

  // Keep one slot free for the closing gap. A frame that doesn't fit, or a
  // carrier-on stretch too long for 16 bits, is dropped rather than truncated.
  if (s_rb.count >= RF_MAX_PULSES - 1 || width > 0xFFFF) {
    s_rb.overflow = true;
    s_rb.count    = 0;
    s_rb.inFrame  = false;
    return;
  }
  s_rb.buf[s_rb.count] = (uint16_t)width;
  s_rb.count = s_rb.count + 1;
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

  drainFrameFromRingBuffer();   // also re-arms the ISR for the next frame

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
// Copy the completed frame into _capture[_captureCount] and re-arm the ISR.
// The ISR is parked while frameReady is set, so the buffer is stable here.
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::drainFrameFromRingBuffer() {
  uint16_t n = s_rb.count;
  if (n > RF_MAX_PULSES) n = RF_MAX_PULSES;
  for (uint16_t i = 0; i < n; i++) _capture[_captureCount][i] = s_rb.buf[i];
  _captureLen[_captureCount] = n;

  s_rb.count      = 0;
  s_rb.inFrame    = false;   // the next frame starts at the next gap
  s_rb.frameReady = false;   // last: un-parks the ISR
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

    // The frame's own closing gap has already been timed above.
    if (RF_TX_GAP_MS > 0 && rep + 1 < RF_TX_REPEATS) delay(RF_TX_GAP_MS);
  }

  // Release the line before leaving TX: out of TX the chip drives GDO0 itself.
  pinMode(_gdo0Pin, INPUT_PULLDOWN);
  _cc.setIdle();
  _state = State::IDLE;
  Serial.printf("[RF] Transmitted %u pulses x %u\n", count, RF_TX_REPEATS);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// ISR attach / detach
// ────────────────────────────────────────────────────────────────────────────
void RFLearn::attachISR() {
  pinMode(_gdo0Pin, INPUT);
  s_gdo0 = (gpio_num_t)_gdo0Pin;
  s_rb.lastEdgeUs = (uint32_t)esp_timer_get_time();
  attachInterrupt(digitalPinToInterrupt(_gdo0Pin), gdo0IsrHandler, CHANGE);
}

void RFLearn::detachISR() {
  detachInterrupt(digitalPinToInterrupt(_gdo0Pin));
}
