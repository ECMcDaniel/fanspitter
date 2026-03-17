#pragma once
#include <Arduino.h>
#include <functional>
#include "cc1101.h"

// ────────────────────────────────────────────────────────────────────────────
// Pulse capture limits
// ────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t RF_MAX_PULSES      = 400;   // max pulses per frame
static constexpr uint32_t RF_FRAME_GAP_US    = 10000; // gap > 10 ms = end-of-frame
static constexpr uint16_t RF_MIN_PULSES      = 10;    // discard frames shorter than this
static constexpr uint8_t  RF_CONFIRM_TOL_PCT = 20;    // ±20% tolerance for confirmation
static constexpr uint32_t RF_LEARN_TIMEOUT_MS = 30000; // max time in learn mode (ms)
static constexpr uint8_t  RF_TX_REPEATS      = 3;     // how many times to replay a signal

// ────────────────────────────────────────────────────────────────────────────
// ISR ring buffer (lives in DRAM, shared between ISR and main loop)
// ────────────────────────────────────────────────────────────────────────────
struct PulseRingBuffer {
  volatile uint16_t buf[RF_MAX_PULSES];
  volatile uint16_t head;       // ISR writes
  volatile uint16_t tail;       // main loop reads
  volatile bool     frameReady; // ISR sets when gap detected
  volatile bool     overflow;   // ISR sets on ring-buffer wraparound
  volatile uint32_t lastEdgeUs; // timestamp of last edge (µs)
};

// ────────────────────────────────────────────────────────────────────────────
// RFLearn — capture & replay class
// ────────────────────────────────────────────────────────────────────────────
class RFLearn {
public:
  enum class State { IDLE, LEARNING, CONFIRMING, TRANSMITTING };

  using DoneCallback = std::function<void(bool ok, const String& reason)>;

  // cc1101 must be initialised (cc1101.begin()) before calling begin() here.
  RFLearn(CC1101& cc1101, uint8_t gdo0Pin);

  // Attach the pin-change ISR and put the CC1101 into RX mode.
  // Call once from setup().
  void begin();

  // Drive the state machine — call on every iteration of loop().
  void process();

  // Request entry into learn mode.
  // Returns false if not currently IDLE (busy).
  bool startLearn();

  // Request signal replay from a pulse array (e.g. loaded from SignalStore).
  // Returns false if not currently IDLE.
  bool startTransmit(const uint16_t* pulses, uint16_t count);

  State getState() const { return _state; }

  // After a successful learn (state returns to IDLE from CONFIRMING),
  // call this to copy the confirmed pulses out.
  // Returns count of pulses, or 0 if no confirmed signal is pending.
  uint16_t getConfirmedPulses(uint16_t* outBuf, uint16_t maxLen) const;

  // Callbacks
  void onLearnDone(DoneCallback cb)    { _learnDoneCb   = cb; }
  void onTransmitDone(DoneCallback cb) { _txDoneCb      = cb; }

  // ── Singleton accessor used by the static ISR ───────────────────────────
  static RFLearn* instance;

private:
  CC1101&  _cc;
  uint8_t  _gdo0Pin;
  State    _state;

  // ── Capture buffers ──────────────────────────────────────────────────────
  uint16_t _capture[2][RF_MAX_PULSES]; // two frames for confirmation
  uint16_t _captureLen[2];
  uint8_t  _captureCount;              // 0, 1, or 2

  // ── Confirmed signal ─────────────────────────────────────────────────────
  uint16_t _confirmedBuf[RF_MAX_PULSES];
  uint16_t _confirmedLen;
  bool     _confirmedPending;          // true until caller calls getConfirmedPulses()

  // ── TX replay ────────────────────────────────────────────────────────────
  const uint16_t* _txPulses;
  uint16_t        _txCount;

  // ── Timing ───────────────────────────────────────────────────────────────
  unsigned long _learnStartMs;

  // ── Callbacks ────────────────────────────────────────────────────────────
  DoneCallback _learnDoneCb;
  DoneCallback _txDoneCb;

  // ── Private helpers ──────────────────────────────────────────────────────
  void drainFrameFromRingBuffer();   // copies ISR → _capture[_captureCount]
  bool confirmFrames();              // compare _capture[0] vs _capture[1]
  void doTransmit();                 // blocking bit-bang TX (repeats RF_TX_REPEATS times)
  void attachISR();
  void detachISR();
};

// ────────────────────────────────────────────────────────────────────────────
// ISR — defined in rf_learn.cpp, declared here so the linker can see it
// ────────────────────────────────────────────────────────────────────────────
void IRAM_ATTR gdo0IsrHandler();
