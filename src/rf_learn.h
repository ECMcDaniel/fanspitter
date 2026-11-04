#pragma once
#include <Arduino.h>
#include <functional>
#include "cc1101.h"

// ────────────────────────────────────────────────────────────────────────────
// Pulse capture limits
// ────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t RF_MAX_PULSES       = 400;   // max pulses per frame
static constexpr uint32_t RF_FRAME_GAP_US     = 10000; // gap > 10 ms = end-of-frame
static constexpr uint16_t RF_GLITCH_US        = 50;    // ignore edges closer than this
static constexpr uint16_t RF_MIN_PULSES       = 10;    // discard frames shorter than this
static constexpr uint8_t  RF_CONFIRM_TOL_PCT  = 20;    // ±20% tolerance for confirmation
static constexpr uint16_t RF_CONFIRM_TOL_MIN_US = 50;  // …but never tighter than 50 µs
static constexpr uint8_t  RF_CONFIRM_MAX_LEN_DIFF = 2; // allowed pulse-count difference
static constexpr uint32_t RF_LEARN_TIMEOUT_MS = 30000; // max time in learn mode (ms)
static constexpr uint8_t  RF_TX_REPEATS       = 3;     // how many times to replay a signal
static constexpr uint32_t RF_TX_GAP_MS        = 20;    // gap between repeats
// Frames longer than this are sent with interrupts enabled (more jitter) so
// the 300 ms interrupt watchdog can never fire.
static constexpr uint32_t RF_TX_MAX_MASKED_US = 200000;

// ────────────────────────────────────────────────────────────────────────────
// RFLearn — capture & replay
//
// The CC1101 is kept in IDLE except while learning (RX) or transmitting (TX),
// and the GDO0 interrupt is only attached while learning. This keeps the
// receiver's noise output from interrupting the CPU all day and keeps GDO0
// quiet (it is a strapping pin on the default wiring).
//
// All methods must be called from the same task (the Arduino loop task).
// ────────────────────────────────────────────────────────────────────────────
class RFLearn {
public:
  enum class State { IDLE, LEARNING, CONFIRMING, TRANSMITTING };

  using DoneCallback = std::function<void(bool ok, const char* reason)>;

  // cc1101 must be initialised (cc1101.begin()) before calling begin() here.
  RFLearn(CC1101& cc1101, uint8_t gdo0Pin);

  // Put the radio in IDLE and configure GDO0. Call once from setup().
  void begin();

  // Drive the state machine — call on every iteration of loop().
  void process();

  // Enter learn mode (radio → RX, ISR attached).
  // Returns false if not currently IDLE.
  bool startLearn();

  // Abort learn mode without invoking the learn callback.
  void cancelLearn();

  // Replay a pulse array (e.g. loaded from SignalStore). Blocking: returns
  // after all repeats have been sent. Returns false if busy or the radio
  // failed to enter TX.
  bool transmit(const uint16_t* pulses, uint16_t count);

  State getState() const { return _state; }

  // After a successful learn, copy the confirmed pulses out.
  // Returns count of pulses, or 0 if no confirmed signal is pending.
  uint16_t getConfirmedPulses(uint16_t* outBuf, uint16_t maxLen) const;

  void onLearnDone(DoneCallback cb) { _learnDoneCb = cb; }

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
  bool     _confirmedPending;

  unsigned long _learnStartMs;
  DoneCallback  _learnDoneCb;

  void finishLearn(bool ok, const char* reason);
  void drainFrameFromRingBuffer();   // copies ISR → _capture[_captureCount]
  bool confirmFrames();              // compare _capture[0] vs _capture[1]
  void stopRx();
  void attachISR();
  void detachISR();
};
