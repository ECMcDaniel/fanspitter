#include "signal_store.h"

// RAII mutex guard
class StoreLock {
public:
  explicit StoreLock(const SignalStore& s) : _m(s._mtx) {
    if (_m) xSemaphoreTake(_m, portMAX_DELAY);
  }
  ~StoreLock() {
    if (_m) xSemaphoreGive(_m);
  }
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;
private:
  SemaphoreHandle_t _m;
};

static void idxKey(char key[3], uint8_t slot) {
  key[0] = 'n';
  key[1] = (char)('0' + slot);
  key[2] = '\0';
}

void SignalStore::begin() {
  if (!_mtx) _mtx = xSemaphoreCreateMutex();
  StoreLock lock(*this);

  Preferences prefs;
  // readOnly=false so the namespace is created on first boot (a read-only
  // open of a missing namespace fails and logs an error).
  prefs.begin("fs_idx", false);
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    char key[3];
    idxKey(key, i);
    _names[i] = prefs.isKey(key) ? prefs.getString(key, "") : String();
  }
  prefs.end();

  Serial.printf("[Store] Loaded %u stored signals\n", countUnlocked());
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i].length() > 0) {
      Serial.printf("[Store]   [%u] %s\n", i, _names[i].c_str());
    }
  }
}

uint8_t SignalStore::count() const {
  StoreLock lock(*this);
  return countUnlocked();
}

void SignalStore::listNames(String names[MAX_SLOTS]) const {
  StoreLock lock(*this);
  for (uint8_t i = 0; i < MAX_SLOTS; i++) names[i] = _names[i];
}

bool SignalStore::save(const String& name, const uint16_t* pulses, uint16_t pulseCount) {
  if (name.length() == 0 || !pulses || pulseCount == 0) return false;
  if (pulseCount > RF_MAX_PULSES) pulseCount = RF_MAX_PULSES;

  StoreLock lock(*this);
  int slot = findSlot(name);
  if (slot < 0) {
    slot = freeSlot();
    if (slot < 0) {
      Serial.println("[Store] ERROR: all slots full");
      return false;
    }
  }

  // Data first, then the index entry, so a power cut never leaves a name
  // pointing at missing data.
  const size_t bytes = pulseCount * sizeof(uint16_t);
  {
    Preferences prefs;
    if (!prefs.begin(slotNS((uint8_t)slot).c_str(), false)) {
      Serial.println("[Store] ERROR: cannot open slot namespace");
      return false;
    }
    bool ok = prefs.putBytes("data", pulses, bytes) == bytes &&
              prefs.putUShort("cnt", pulseCount) == sizeof(uint16_t);
    prefs.end();
    if (!ok) {
      Serial.println("[Store] ERROR: NVS write failed (flash full?)");
      return false;
    }
  }
  {
    Preferences idx;
    idx.begin("fs_idx", false);
    char key[3];
    idxKey(key, (uint8_t)slot);
    bool ok = idx.putString(key, name) == name.length();
    idx.end();
    if (!ok) {
      Serial.println("[Store] ERROR: NVS index write failed");
      return false;
    }
  }
  _names[slot] = name;

  Serial.printf("[Store] Saved \"%s\" to slot %d (%u pulses)\n",
                name.c_str(), slot, pulseCount);
  return true;
}

uint16_t SignalStore::load(const String& name, uint16_t* outBuf, uint16_t maxLen) const {
  StoreLock lock(*this);
  int slot = findSlot(name);
  if (slot < 0) {
    Serial.printf("[Store] Not found: \"%s\"\n", name.c_str());
    return 0;
  }

  Preferences prefs;
  if (!prefs.begin(slotNS((uint8_t)slot).c_str(), true)) return 0;
  uint16_t cnt = prefs.getUShort("cnt", 0);
  uint16_t n = (cnt < maxLen) ? cnt : maxLen;
  size_t got = (n > 0) ? prefs.getBytes("data", outBuf, n * sizeof(uint16_t)) : 0;
  prefs.end();

  if (got != n * sizeof(uint16_t)) {
    Serial.printf("[Store] ERROR: slot \"%s\" data is corrupt\n", name.c_str());
    return 0;
  }
  return n;
}

bool SignalStore::remove(const String& name) {
  StoreLock lock(*this);
  int slot = findSlot(name);
  if (slot < 0) {
    Serial.printf("[Store] Delete failed: \"%s\" not found\n", name.c_str());
    return false;
  }

  // Index first so a power cut leaves orphaned data (harmless) rather than
  // a name with no data.
  {
    Preferences idx;
    idx.begin("fs_idx", false);
    char key[3];
    idxKey(key, (uint8_t)slot);
    idx.remove(key);
    idx.end();
  }
  {
    Preferences prefs;
    prefs.begin(slotNS((uint8_t)slot).c_str(), false);
    prefs.clear();
    prefs.end();
  }
  _names[slot] = "";

  Serial.printf("[Store] Deleted \"%s\" from slot %d\n", name.c_str(), slot);
  return true;
}

bool SignalStore::exists(const String& name) const {
  StoreLock lock(*this);
  return findSlot(name) >= 0;
}

// ── Unlocked helpers ────────────────────────────────────────────────────────
uint8_t SignalStore::countUnlocked() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_SLOTS; i++) {
    if (_names[i].length() > 0) n++;
  }
  return n;
}

int SignalStore::findSlot(const String& name) const {
  if (name.length() == 0) return -1;
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
  String s = "fs_s";
  s += (char)('0' + idx);
  return s;
}
