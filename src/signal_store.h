#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "rf_learn.h"  // for RF_MAX_PULSES

// ────────────────────────────────────────────────────────────────────────────
// SignalStore — NVS-backed storage for up to 8 named signal slots
//
// NVS layout:
//   Namespace "fs_idx"
//     "n0" … "n7"  String   slot names  (empty = unused)
//
//   Namespace "fs_s0" … "fs_s7"
//     "cnt"         UShort   pulse count
//     "data"        Bytes    raw uint16_t[] pulse widths
// ────────────────────────────────────────────────────────────────────────────
class SignalStore {
public:
  static constexpr uint8_t MAX_SLOTS = 8;

  SignalStore() = default;

  // Open NVS namespaces; call once from setup().
  void begin();

  // Return number of occupied slots.
  uint8_t count() const;

  // Copy all slot names into names[0…MAX_SLOTS-1].
  // Empty String entries are unused slots.
  void listNames(String names[MAX_SLOTS]) const;

  // Save a signal under name.
  // If name already exists, its data is overwritten.
  // Returns false if name is new and all 8 slots are full.
  bool save(const String& name, const uint16_t* pulses, uint16_t pulseCount);

  // Load a signal into outBuf.
  // Returns the number of pulses copied, or 0 if name not found.
  uint16_t load(const String& name, uint16_t* outBuf, uint16_t maxLen) const;

  // Delete slot by name.
  // Returns false if name not found.
  bool remove(const String& name);

  // Return true if a slot with that name exists.
  bool exists(const String& name) const;

private:
  // Find slot index (0‥7) for a name, or -1 if not found.
  int findSlot(const String& name) const;

  // Return first slot index with an empty name, or -1 if all occupied.
  int freeSlot() const;

  // Build NVS namespace string for slot index: "fs_s0" … "fs_s7".
  static String slotNS(uint8_t idx);

  // Slot names cached in RAM for fast lookup.
  String _names[MAX_SLOTS];
};
