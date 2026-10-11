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

// The HAL guide's §5 bring-up values are 0x57 (100 Hz) and 0x80 (+/-2 g), and
// they are a data-path smoke test aimed at tilt sensing. Both are wrong for
// impact work, and the second one is wrong on its own terms:
//
//   * 0x80 leaves CTRL_REG4 bit 3 (HR, high resolution) CLEAR, which is 10-bit
//     mode. §5 then describes the output as "12-bit, 1 count = 1 mg" — those
//     two statements cannot both be true. In 10-bit mode the value only moves
//     in steps of 4, so you silently get 4 mg of real resolution while the
//     code claims 1 mg. Confirmed against the Silan SC7A20 datasheet Rev 1.3,
//     which publishes no sensitivity figure at all for non-HR mode.
//   * +/-2 g has no headroom for a knock. Flat on a table Z already carries
//     +1 g of gravity, leaving 1 g of dynamic range on the exact axis an
//     impact excites hardest, and a knuckle knock reads 3-5 g.
//   * 100 Hz is too slow to characterise an impact: the envelope of a knock to
//     a badge lying on a desk lasts only 5-15 ms, which is ONE sample at
//     100 Hz. Every discriminator except raw amplitude becomes impossible.
//
// 400 Hz is this part's real maximum; the LIS3DH's 1.344 kHz does not apply.
constexpr uint8_t CTRL_REG1_VALUE = 0x77;  // 400 Hz, all three axes on
constexpr uint8_t CTRL_REG4_VALUE = 0xA8;  // BDU | FS=+/-8 g | HR (12-bit)

// Sensitivity in HR mode, from the Silan datasheet: 1/2/4/8 mg per count at
// +/-2/4/8/16 g. Tied to CTRL_REG4_VALUE above so the two cannot drift apart.
// (Note for anyone cross-checking against ST: the LIS3DH is 12 mg/count at
// +/-16 g, this part is 8. Linux's st_accel driver uses ST's value for the
// SC7A20 by analogy and its own comment admits "not an ST part".)
constexpr int16_t MG_PER_COUNT = 4;

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
  present_    = readRegs(REG_WHO_AM_I, &who, 1) && who == WHO_AM_I_EXPECTED;
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

  // Little-endian pairs, 12-bit left-justified (§5): counts = raw >> 4. The
  // arithmetic shift on a signed int16_t sign-extends, so negative axes come
  // back correctly — this is the step that silently works at +1 g and breaks
  // at -1 g if you shift an unsigned value instead.
  //
  // Counts are then scaled to real milligravities. At +/-8 g HR one count is
  // 4 mg, so the shifted count is NOT already mg (the guide's claim only held
  // for its own +/-2 g setting, and then only if HR had been set). Full scale
  // is +/-8192 mg, comfortably inside int16_t.
  auto axis = [](uint8_t lo, uint8_t hi) -> int16_t {
    const int16_t full = static_cast<int16_t>(static_cast<uint16_t>(lo) |
                                              (static_cast<uint16_t>(hi) << 8));
    return static_cast<int16_t>((full >> 4) * MG_PER_COUNT);
  };

  r.x_mg  = axis(raw[0], raw[1]);
  r.y_mg  = axis(raw[2], raw[3]);
  r.z_mg  = axis(raw[4], raw[5]);
  r.valid = true;
  return r;
}

}  // namespace badge
