#pragma once
// ────────────────────────────────────────────────────────────────────────────
// Build-time configuration. Every value can be overridden from platformio.ini
// with a build flag, e.g.  -DPIN_GDO0=4
// ────────────────────────────────────────────────────────────────────────────

// CC1101 wiring (SPI uses the default VSPI pins: MOSI=23, MISO=19, SCK=18)
#ifndef PIN_CS
#define PIN_CS 5
#endif

// GDO0 data line. GPIO2 is an ESP32 strapping pin and drives the on-board LED
// on many DevKits; if serial flashing is flaky, rewire to GPIO4 and set
// -DPIN_GDO0=4.
#ifndef PIN_GDO0
#define PIN_GDO0 2
#endif

#ifndef PIN_MISO
#define PIN_MISO MISO
#endif

// Hold this button for FACTORY_RESET_HOLD_MS to wipe WiFi + MQTT settings
// (learned signals are kept). GPIO0 is the BOOT button on most DevKits.
// Set to -1 to disable.
#ifndef PIN_RESET_BTN
#define PIN_RESET_BTN 0
#endif
#ifndef FACTORY_RESET_HOLD_MS
#define FACTORY_RESET_HOLD_MS 5000
#endif

// Network identity: DHCP hostname, mDNS name (<name>.local), OTA hostname.
#ifndef DEVICE_HOSTNAME
#define DEVICE_HOSTNAME "fanspitter"
#endif

// Captive-portal AP name used when no WiFi credentials work.
#ifndef SETUP_AP_NAME
#define SETUP_AP_NAME "FanSpitter-Setup"
#endif

// Injected by scripts/version.py from `git describe`.
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// Reboot if WiFi has been down this long (and no one is using the portal).
#ifndef WIFI_DOWN_REBOOT_MS
#define WIFI_DOWN_REBOOT_MS (10UL * 60UL * 1000UL)
#endif

// Task-watchdog timeout for the main loop. Long enough to cover the slowest
// legitimate blocking call (WiFiManager's 20 s connect after a portal save,
// a bounded MQTT connect, a full RF transmit).
#ifndef LOOP_WDT_TIMEOUT_MS
#define LOOP_WDT_TIMEOUT_MS 60000
#endif

// How often status is re-published to MQTT even if nothing changed.
#ifndef STATUS_HEARTBEAT_MS
#define STATUS_HEARTBEAT_MS 60000UL
#endif
