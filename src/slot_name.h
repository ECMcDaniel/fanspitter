#pragma once
// Pure, dependency-free helpers so they can be unit-tested on the host
// (see test/test_logic).
#include <stddef.h>

// Slot names are used as MQTT payloads, NVS values and HTML text, so they are
// restricted to a safe character set: 1..SLOT_NAME_MAX of [A-Za-z0-9_-].
static constexpr size_t SLOT_NAME_MAX = 32;

inline bool isValidSlotName(const char* s, size_t len) {
  if (!s || len == 0 || len > SLOT_NAME_MAX) return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}
