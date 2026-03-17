#pragma once
#include <Arduino.h>
#include <SPI.h>

// ────────────────────────────────────────────────────────────────────────────
// Default pin assignments (VSPI on ESP32)
// ────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t CC1101_DEFAULT_CS   = 5;
static constexpr uint8_t CC1101_DEFAULT_GDO0 = 2;
static constexpr uint8_t CC1101_MISO_PIN     = 19;

// ────────────────────────────────────────────────────────────────────────────
// Configuration registers (write via single/burst byte)
// ────────────────────────────────────────────────────────────────────────────
namespace CC1101Reg {
  constexpr uint8_t IOCFG0   = 0x02;
  constexpr uint8_t IOCFG2   = 0x00;
  constexpr uint8_t PKTCTRL0 = 0x08;
  constexpr uint8_t FSCTRL1  = 0x0B;
  constexpr uint8_t FREQ2    = 0x0D;
  constexpr uint8_t FREQ1    = 0x0E;
  constexpr uint8_t FREQ0    = 0x0F;
  constexpr uint8_t MDMCFG4  = 0x10;
  constexpr uint8_t MDMCFG3  = 0x11;
  constexpr uint8_t MDMCFG2  = 0x12;
  constexpr uint8_t MDMCFG1  = 0x13;
  constexpr uint8_t MCSM1    = 0x17;
  constexpr uint8_t MCSM0    = 0x18;
  constexpr uint8_t AGCCTRL2 = 0x1B;
  constexpr uint8_t AGCCTRL1 = 0x1C;
  constexpr uint8_t AGCCTRL0 = 0x1D;
  constexpr uint8_t FREND0   = 0x22;
  constexpr uint8_t FSCAL3   = 0x23;
  constexpr uint8_t FSCAL2   = 0x24;
  constexpr uint8_t FSCAL1   = 0x25;
  constexpr uint8_t FSCAL0   = 0x26;
  constexpr uint8_t TEST2    = 0x2C;
  constexpr uint8_t TEST1    = 0x2D;
  constexpr uint8_t TEST0    = 0x2E;
  constexpr uint8_t PATABLE  = 0x3E;  // burst-write address for PA table
}

// ────────────────────────────────────────────────────────────────────────────
// Status registers (read-only; require burst-flag R/W=1, Burst=1)
// ────────────────────────────────────────────────────────────────────────────
namespace CC1101Status {
  constexpr uint8_t PARTNUM   = 0x30;
  constexpr uint8_t VERSION   = 0x31;
  constexpr uint8_t RSSI      = 0x34;
  constexpr uint8_t MARCSTATE = 0x35;
}

// ────────────────────────────────────────────────────────────────────────────
// Strobe commands (single-byte, R/W=1, Burst=0)
// ────────────────────────────────────────────────────────────────────────────
namespace CC1101Strobe {
  constexpr uint8_t SRES  = 0x30;  // software reset
  constexpr uint8_t SCAL  = 0x33;  // synthesiser calibrate
  constexpr uint8_t SRX   = 0x34;  // enter RX
  constexpr uint8_t STX   = 0x35;  // enter TX
  constexpr uint8_t SIDLE = 0x36;  // exit RX/TX, go IDLE
  constexpr uint8_t SFRX  = 0x3A;  // flush RX FIFO
  constexpr uint8_t SFTX  = 0x3B;  // flush TX FIFO
  constexpr uint8_t SNOP  = 0x3D;  // no-op / read chip status
}

// ────────────────────────────────────────────────────────────────────────────
// IOCFG0 signal selections used by this firmware
// ────────────────────────────────────────────────────────────────────────────
constexpr uint8_t IOCFG0_ASYNC_RX = 0x0C;  // demodulated serial output
constexpr uint8_t IOCFG0_ASYNC_TX = 0x0B;  // serial data input (driven by MCU)

// ────────────────────────────────────────────────────────────────────────────
// CC1101 driver class
// ────────────────────────────────────────────────────────────────────────────
class CC1101 {
public:
  // csPin:   SPI chip-select (active low)
  // gdo0Pin: data pin — output from CC1101 in RX, input to CC1101 (MCU-driven) in TX
  // misoPin: needed for "chip ready" polling; must match the SPI bus MISO
  CC1101(uint8_t csPin   = CC1101_DEFAULT_CS,
         uint8_t gdo0Pin = CC1101_DEFAULT_GDO0,
         uint8_t misoPin = CC1101_MISO_PIN);

  // Initialise: reset, verify PARTNUM/VERSION, write register table, calibrate.
  // SPI.begin() must be called before this.
  // Returns false if the part-number / version check fails (wrong wiring, dead chip).
  bool begin();

  // Switch to RX: set IOCFG0 = ASYNC_RX, issue SRX.
  // GDO0 becomes an OUTPUT from the CC1101 carrying raw OOK pulse data.
  void enterRX();

  // Switch to TX: set IOCFG0 = ASYNC_TX, issue STX.
  // GDO0 must then be driven as an OUTPUT by the MCU to inject raw OOK data.
  void enterTX();

  // Place the chip in IDLE state; safe to call from any state.
  void setIdle();

  // Read MARCSTATE register (0x01=IDLE, 0x0D=RX, 0x13=TX, ...).
  uint8_t getMarcState();

  // Read RSSI and convert to signed dBm per CC1101 datasheet formula.
  int8_t getRSSI();

private:
  uint8_t _csPin;
  uint8_t _gdo0Pin;
  uint8_t _misoPin;

  // ── SPI primitives ──────────────────────────────────────────────────────
  void    spiWriteReg(uint8_t addr, uint8_t val);
  uint8_t spiReadReg(uint8_t addr);
  uint8_t spiReadStatusReg(uint8_t addr);        // uses burst flag
  void    spiStrobe(uint8_t cmd);
  void    spiWriteBurst(uint8_t addr, const uint8_t* data, uint8_t len);

  // ── Internal ─────────────────────────────────────────────────────────────
  void assertCS();
  void deassertCS();
  void waitChipReady();      // spin until MISO goes low (CC1101 ready)
  void reset();
  void loadRegisterTable();  // write all rows of CONFIG_TABLE

  // Static register table: pairs of { address, value }
  static const uint8_t CONFIG_TABLE[][2];
  static const size_t  CONFIG_TABLE_LEN;
};
