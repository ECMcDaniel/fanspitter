#pragma once
#include <stdint.h>

// Result of submitting a command from the web UI or MQTT. Commands are
// validated in the caller's task and then executed by the main loop task.
enum class CmdResult : uint8_t {
  OK,
  INVALID_NAME,
  BUSY,
  NOT_FOUND,
  STORE_FULL,
  QUEUE_FULL,
  RF_UNAVAILABLE,
};

inline const char* cmdResultStr(CmdResult r) {
  switch (r) {
    case CmdResult::OK:             return "ok";
    case CmdResult::INVALID_NAME:   return "invalid slot name (1-32 chars: A-Z a-z 0-9 _ -)";
    case CmdResult::BUSY:           return "busy";
    case CmdResult::NOT_FOUND:      return "slot not found";
    case CmdResult::STORE_FULL:     return "all slots are full";
    case CmdResult::QUEUE_FULL:     return "too many pending commands";
    case CmdResult::RF_UNAVAILABLE: return "RF module not available";
  }
  return "error";
}
