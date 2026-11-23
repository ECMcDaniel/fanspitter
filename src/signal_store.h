#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "rf_learn.h"  // for RF_MAX_PULSES

// ────────────────────────────────────────────────────────────────────────────
// SignalStore — NVS-backed storage for up to MAX_SLOTS named signal slots
//
// NVS layout:
//   Namespace "fs_idx"
//     "n0" … "n31" String   slot names  (empty = unused)
//
//   Namespace "fs_s0" … "fs_s31"
//     "cnt"         UShort   pulse count
//     "data"        Bytes    raw uint16_t[] pulse widths
//
// Thread-safe: every public method takes an internal mutex, so the web
// server task can read while the loop task writes.
// ────────────────────────────────────────────────────────────────────────────
class SignalStore {
public:
  // 32 covers two fans with a full remote each. Slots are stored at their
  // real length (a 66-pulse signal costs about 11 NVS entries), and the 20 KB
  // NVS partition holds roughly 500 entries shared with WiFi and settings.
  // begin() logs the entries in use so the headroom is visible.
  static constexpr uint8_t MAX_SLOTS = 32;

  SignalStore() = default;

  // Create the mutex and load slot names from NVS; call once from setup().
  void begin();

  uint8_t count() const;
  bool    isFull() const { return count() >= MAX_SLOTS; }

  // Copy all slot names into names[0…MAX_SLOTS-1]. Empty entries are unused.
  void listNames(String names[MAX_SLOTS]) const;

  // Save a signal under name (overwrites if it exists).
  // Returns false if the name is new and all slots are full, or NVS fails.
  bool save(const String& name, const uint16_t* pulses, uint16_t pulseCount);

  // Load a signal into outBuf. Returns pulses copied, or 0 if not found.
  uint16_t load(const String& name, uint16_t* outBuf, uint16_t maxLen) const;

  // Delete slot by name. Returns false if not found.
  bool remove(const String& name);

  bool exists(const String& name) const;

private:
  // Unlocked helpers — caller must hold _mtx.
  int    findSlot(const String& name) const;
  int    freeSlot() const;
  uint8_t countUnlocked() const;
  static String slotNS(uint8_t idx);

  String _names[MAX_SLOTS];
  mutable SemaphoreHandle_t _mtx = nullptr;

  friend class StoreLock;
};
