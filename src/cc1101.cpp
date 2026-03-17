#include "cc1101.h"

// ────────────────────────────────────────────────────────────────────────────
// Register table — written top-to-bottom by loadRegisterTable()
// Generated from SmartRF Studio 7 for 26 MHz crystal, 303.002 MHz, OOK,
// async serial mode (PKT_FORMAT = 11b), infinite packet length.
// ────────────────────────────────────────────────────────────────────────────
const uint8_t CC1101::CONFIG_TABLE[][2] = {
  //  address               value     comment
  { CC1101Reg::IOCFG0,   0x0C },  // GDO0 = async serial data output (RX default)
  { CC1101Reg::IOCFG2,   0x29 },  // GDO2 = chip-ready signal (optional, unused)
  { CC1101Reg::PKTCTRL0, 0x32 },  // async serial mode, infinite packet, no CRC
  { CC1101Reg::FSCTRL1,  0x06 },  // IF frequency = 152.3 kHz
  { CC1101Reg::FREQ2,    0x0B },  // ─┐
  { CC1101Reg::FREQ1,    0xA7 },  //  │ 303.002 MHz (26 MHz XO)
  { CC1101Reg::FREQ0,    0x6E },  // ─┘
  { CC1101Reg::MDMCFG4,  0xF6 },  // BW ~67 kHz (wide enough for OOK pulses)
  { CC1101Reg::MDMCFG3,  0x83 },  // data-rate mantissa (< not used in async mode)
  { CC1101Reg::MDMCFG2,  0x30 },  // OOK modulation, no sync word, no preamble detect
  { CC1101Reg::MDMCFG1,  0x22 },  // 4 preamble bytes, no FEC
  { CC1101Reg::MCSM1,    0x30 },  // after RX → stay RX; after TX → IDLE
  { CC1101Reg::MCSM0,    0x18 },  // auto-calibrate on every IDLE→RX/TX transition
  { CC1101Reg::AGCCTRL2, 0xC7 },  // OOK MAGN_TARGET = 33 dB
  { CC1101Reg::AGCCTRL1, 0x40 },  // CS relative threshold
  { CC1101Reg::AGCCTRL0, 0xB2 },  // OOK AGC hysteresis / wait time
  { CC1101Reg::FREND0,   0x11 },  // OOK TX: PA table index 0 for '1', 0 for '0'
  { CC1101Reg::FSCAL3,   0xEA },  // ─┐
  { CC1101Reg::FSCAL2,   0x2A },  //  │ Calibration constants from SmartRF Studio
  { CC1101Reg::FSCAL1,   0x00 },  //  │ (valid for 26 MHz XO, 303 MHz)
  { CC1101Reg::FSCAL0,   0x1F },  // ─┘
  { CC1101Reg::TEST2,    0x81 },  // ─┐ Test registers — values from SmartRF Studio
  { CC1101Reg::TEST1,    0x35 },  //  │
  { CC1101Reg::TEST0,    0x09 },  // ─┘
};
const size_t CC1101::CONFIG_TABLE_LEN =
  sizeof(CC1101::CONFIG_TABLE) / sizeof(CC1101::CONFIG_TABLE[0]);

// ────────────────────────────────────────────────────────────────────────────
// Constructor
// ────────────────────────────────────────────────────────────────────────────
CC1101::CC1101(uint8_t csPin, uint8_t gdo0Pin, uint8_t misoPin)
  : _csPin(csPin), _gdo0Pin(gdo0Pin), _misoPin(misoPin) {}

// ────────────────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────────────────
bool CC1101::begin() {
  pinMode(_csPin, OUTPUT);
  deassertCS();

  reset();

  uint8_t partnum = spiReadStatusReg(CC1101Status::PARTNUM);
  uint8_t version = spiReadStatusReg(CC1101Status::VERSION);
  Serial.printf("[CC1101] PARTNUM=0x%02X VERSION=0x%02X\n", partnum, version);

  // Known valid: PARTNUM==0x00, VERSION==0x14 (most modules)
  // Some clones report VERSION==0x04 — accept either.
  if (partnum != 0x00 || (version != 0x14 && version != 0x04)) {
    Serial.println("[CC1101] ERROR: unexpected chip ID — check wiring");
    return false;
  }
  Serial.println("[CC1101] OK");

  loadRegisterTable();

  // Write maximum TX output power (0xC0 ≈ +10 dBm) to PATABLE[0]
  const uint8_t pa = 0xC0;
  spiWriteBurst(CC1101Reg::PATABLE, &pa, 1);

  // Calibrate the frequency synthesiser
  spiStrobe(CC1101Strobe::SCAL);
  delay(2);  // calibration takes < 1 ms but give margin

  return true;
}

void CC1101::enterRX() {
  spiWriteReg(CC1101Reg::IOCFG0, IOCFG0_ASYNC_RX);
  spiStrobe(CC1101Strobe::SRX);
}

void CC1101::enterTX() {
  spiWriteReg(CC1101Reg::IOCFG0, IOCFG0_ASYNC_TX);
  spiStrobe(CC1101Strobe::STX);
}

void CC1101::setIdle() {
  spiStrobe(CC1101Strobe::SIDLE);
}

uint8_t CC1101::getMarcState() {
  return spiReadStatusReg(CC1101Status::MARCSTATE) & 0x1F;
}

int8_t CC1101::getRSSI() {
  uint8_t raw = spiReadStatusReg(CC1101Status::RSSI);
  // Per CC1101 datasheet §10.3: RSSI_dBm = RSSI_dec/2 - 74  (if RSSI_dec >= 128)
  //                                       = (RSSI_dec - 256)/2 - 74  (if < 128)
  int16_t rssi_dec = (int16_t)raw;
  if (rssi_dec >= 128) {
    return (int8_t)((rssi_dec / 2) - 74);
  } else {
    return (int8_t)(((rssi_dec - 256) / 2) - 74);
  }
}

// ────────────────────────────────────────────────────────────────────────────
// Private — SPI primitives
// ────────────────────────────────────────────────────────────────────────────
void CC1101::assertCS() {
  digitalWrite(_csPin, LOW);
  // The CC1101 requires MISO to go low before the first clock edge.
  // MISO is held high internally while the chip is not ready (POR, strobe, etc.).
  waitChipReady();
}

void CC1101::deassertCS() {
  digitalWrite(_csPin, HIGH);
}

void CC1101::waitChipReady() {
  // Spin until MISO goes low. This is a documented requirement; skipping it
  // causes silent register corruption for any SPI access after a strobe.
  uint32_t deadline = micros() + 2000;  // 2 ms timeout
  while (digitalRead(_misoPin) == HIGH) {
    if ((int32_t)(micros() - deadline) > 0) {
      Serial.println("[CC1101] WARN: chip-ready timeout");
      break;
    }
  }
}

void CC1101::spiWriteReg(uint8_t addr, uint8_t val) {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer(addr & 0x3F);  // R/W=0, Burst=0
  SPI.transfer(val);
  deassertCS();
  SPI.endTransaction();
}

uint8_t CC1101::spiReadReg(uint8_t addr) {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer((addr & 0x3F) | 0x80);  // R/W=1, Burst=0
  uint8_t val = SPI.transfer(0x00);
  deassertCS();
  SPI.endTransaction();
  return val;
}

uint8_t CC1101::spiReadStatusReg(uint8_t addr) {
  // Status registers require R/W=1, Burst=1 (two high bits set)
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer((addr & 0x3F) | 0xC0);
  uint8_t val = SPI.transfer(0x00);
  deassertCS();
  SPI.endTransaction();
  return val;
}

void CC1101::spiStrobe(uint8_t cmd) {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer(cmd);
  deassertCS();
  SPI.endTransaction();
}

void CC1101::spiWriteBurst(uint8_t addr, const uint8_t* data, uint8_t len) {
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer((addr & 0x3F) | 0x40);  // R/W=0, Burst=1
  for (uint8_t i = 0; i < len; i++) {
    SPI.transfer(data[i]);
  }
  deassertCS();
  SPI.endTransaction();
}

// ────────────────────────────────────────────────────────────────────────────
// Private — internal helpers
// ────────────────────────────────────────────────────────────────────────────
void CC1101::reset() {
  // Power-on reset sequence per CC1101 datasheet §10.1
  deassertCS();
  delayMicroseconds(5);
  assertCS();
  delayMicroseconds(10);
  deassertCS();
  delayMicroseconds(41);  // minimum 40 µs before asserting CS for SRES

  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  assertCS();
  SPI.transfer(CC1101Strobe::SRES);
  // Wait for MISO to go low — CC1101 holds it high during reset
  uint32_t deadline = micros() + 5000;
  while (digitalRead(_misoPin) == HIGH) {
    if ((int32_t)(micros() - deadline) > 0) break;
  }
  deassertCS();
  SPI.endTransaction();
  delay(1);
}

void CC1101::loadRegisterTable() {
  for (size_t i = 0; i < CONFIG_TABLE_LEN; i++) {
    spiWriteReg(CONFIG_TABLE[i][0], CONFIG_TABLE[i][1]);
  }
}
