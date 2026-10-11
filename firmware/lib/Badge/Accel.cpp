#include "Accel.h"

#include <Wire.h>

#include "badge_pins.h"

namespace badge {
namespace {
// Register map, HAL guide §5.
constexpr uint8_t REG_WHO_AM_I  = 0x0F;
constexpr uint8_t REG_CTRL_REG1 = 0x20;
constexpr uint8_t REG_CTRL_REG4 = 0x23;
constexpr uint8_t REG_STATUS    = 0x27;
constexpr uint8_t REG_OUT_X_L   = 0x28;

constexpr uint8_t WHO_AM_I_EXPECTED = 0x11;

constexpr uint8_t CTRL_REG1_VALUE = 0x57;  // 100 Hz, all three axes on
constexpr uint8_t CTRL_REG4_VALUE = 0x80;  // block-data-update, LE, +/-2 g

constexpr uint8_t STATUS_ZYXDA_BIT = 0x08;  // bit 3: new data on all axes

// §5: multi-byte reads need bit 7 of the register address set for
// auto-increment. Without it you read the same register six times and the
// sensor looks broken while answering perfectly.
constexpr uint8_t AUTO_INCREMENT = 0x80;
}  // namespace

bool Accel::begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);

  // The bounded timeout §5 insists on. Default behaviour can stall far longer,
  // which against a 10 ms poll loop reads to the user as a frozen badge.
  Wire.setTimeOut(static_cast<uint16_t>(I2C_TIMEOUT_MS));

  uint8_t who = 0;
  present_ = readRegs(REG_WHO_AM_I, &who, 1) && who == WHO_AM_I_EXPECTED;
  if (!present_) return false;

  present_ = writeReg(REG_CTRL_REG1, CTRL_REG1_VALUE) &&
             writeReg(REG_CTRL_REG4, CTRL_REG4_VALUE);
  return present_;
}

bool Accel::writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ADDR_ACCEL);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) {
    ++errors_;
    return false;
  }
  return true;
}

bool Accel::readRegs(uint8_t reg, uint8_t* out, uint8_t count) {
  if (out == nullptr || count == 0) return false;

  Wire.beginTransmission(ADDR_ACCEL);
  Wire.write(count > 1 ? static_cast<uint8_t>(reg | AUTO_INCREMENT) : reg);
  if (Wire.endTransmission(false) != 0) {  // keep the bus for the read
    ++errors_;
    return false;
  }

  const uint8_t got = static_cast<uint8_t>(
      Wire.requestFrom(static_cast<int>(ADDR_ACCEL), static_cast<int>(count)));
  if (got != count) {
    ++errors_;
    return false;
  }

  for (uint8_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

AccelReading Accel::read() {
  AccelReading r;
  if (!present_) return r;

  uint8_t status = 0;
  if (!readRegs(REG_STATUS, &status, 1)) return r;
  if ((status & STATUS_ZYXDA_BIT) == 0) return r;  // nothing new yet

  uint8_t raw[6] = {0};
  if (!readRegs(REG_OUT_X_L, raw, sizeof(raw))) return r;

  // Little-endian pairs, 12-bit left-justified. §5: counts = raw >> 4, and at
  // +/-2 g one count is 1 mg — so the shifted count already IS milligravities.
  auto axis = [](uint8_t lo, uint8_t hi) -> int16_t {
    const int16_t full = static_cast<int16_t>(static_cast<uint16_t>(lo) |
                                              (static_cast<uint16_t>(hi) << 8));
    return static_cast<int16_t>(full >> 4);
  };

  r.x_mg  = axis(raw[0], raw[1]);
  r.y_mg  = axis(raw[2], raw[3]);
  r.z_mg  = axis(raw[4], raw[5]);
  r.valid = true;
  return r;
}

}  // namespace badge
