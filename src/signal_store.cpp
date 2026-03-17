#include "signal_store.h"

// ────────────────────────────────────────────────────────────────────────────
// begin() — load names from NVS into RAM cache
// ────────────────────────────────────────────────────────────────────────────
void SignalStore::begin() {
  Preferences prefs;
  prefs.begin("fs_idx", /*readOnly=*/true);
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    char key[3] = {'n', (char)('0' + i), '\0'};
    _names[i] = prefs.getString(key, "");
  }
  prefs.end();

  Serial.printf("[Store] Loaded %u stored signals\n", count());
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i].length() > 0) {
      Serial.printf("[Store]   [%u] %s\n", i, _names[i].c_str());
    }
  }
}

// ────────────────────────────────────────────────────────────────────────────
// count()
// ────────────────────────────────────────────────────────────────────────────
uint8_t SignalStore::count() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i].length() > 0) n++;
  }
  return n;
}

// ────────────────────────────────────────────────────────────────────────────
// listNames()
// ────────────────────────────────────────────────────────────────────────────
void SignalStore::listNames(String names[MAX_SLOTS]) const {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    names[i] = _names[i];
  }
}

// ────────────────────────────────────────────────────────────────────────────
// save()
// ────────────────────────────────────────────────────────────────────────────
bool SignalStore::save(const String& name, const uint16_t* pulses, uint16_t pulseCount) {
  if (name.length() == 0 || !pulses || pulseCount == 0) return false;

  int slot = findSlot(name);
  if (slot < 0) {
    slot = freeSlot();
    if (slot < 0) {
      Serial.println("[Store] ERROR: all 8 slots full");
      return false;
    }
  }

  // Write pulse data into the slot namespace
  String ns = slotNS((uint8_t)slot);
  {
    Preferences prefs;
    prefs.begin(ns.c_str(), /*readOnly=*/false);
    prefs.putUShort("cnt", pulseCount);
    prefs.putBytes("data", pulses, pulseCount * sizeof(uint16_t));
    prefs.end();
  }

  // Update the name index in NVS and RAM cache
  {
    Preferences idx;
    idx.begin("fs_idx", false);
    char key[3] = {'n', (char)('0' + slot), '\0'};
    idx.putString(key, name);
    idx.end();
  }
  _names[slot] = name;

  Serial.printf("[Store] Saved \"%s\" to slot %d (%u pulses)\n",
                name.c_str(), slot, pulseCount);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// load()
// ────────────────────────────────────────────────────────────────────────────
uint16_t SignalStore::load(const String& name, uint16_t* outBuf, uint16_t maxLen) const {
  int slot = findSlot(name);
  if (slot < 0) {
    Serial.printf("[Store] Not found: \"%s\"\n", name.c_str());
    return 0;
  }

  String ns = slotNS((uint8_t)slot);
  Preferences prefs;
  prefs.begin(ns.c_str(), /*readOnly=*/true);
  uint16_t cnt = prefs.getUShort("cnt", 0);
  if (cnt == 0) {
    prefs.end();
    return 0;
  }
  uint16_t n = (cnt < maxLen) ? cnt : maxLen;
  prefs.getBytes("data", outBuf, n * sizeof(uint16_t));
  prefs.end();

  return n;
}

// ────────────────────────────────────────────────────────────────────────────
// remove()
// ────────────────────────────────────────────────────────────────────────────
bool SignalStore::remove(const String& name) {
  int slot = findSlot(name);
  if (slot < 0) {
    Serial.printf("[Store] Delete failed: \"%s\" not found\n", name.c_str());
    return false;
  }

  // Clear the slot data
  String ns = slotNS((uint8_t)slot);
  {
    Preferences prefs;
    prefs.begin(ns.c_str(), false);
    prefs.clear();
    prefs.end();
  }

  // Clear the name in the index
  {
    Preferences idx;
    idx.begin("fs_idx", false);
    char key[3] = {'n', (char)('0' + slot), '\0'};
    idx.putString(key, "");
    idx.end();
  }
  _names[slot] = "";

  Serial.printf("[Store] Deleted \"%s\" from slot %d\n", name.c_str(), slot);
  return true;
}

// ────────────────────────────────────────────────────────────────────────────
// exists()
// ────────────────────────────────────────────────────────────────────────────
bool SignalStore::exists(const String& name) const {
  return findSlot(name) >= 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Private helpers
// ────────────────────────────────────────────────────────────────────────────
int SignalStore::findSlot(const String& name) const {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i] == name) return (int)i;
  }
  return -1;
}

int SignalStore::freeSlot() const {
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i].length() == 0) return (int)i;
  }
  return -1;
}

String SignalStore::slotNS(uint8_t idx) {
  // "fs_s0" … "fs_s7" — NVS namespace names must be ≤ 15 chars
  String s = "fs_s";
  s += (char)('0' + idx);
  return s;
}
