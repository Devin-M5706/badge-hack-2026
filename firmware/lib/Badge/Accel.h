// Accel.h — SC7A20 accelerometer on the shared I2C bus (HAL guide §5).
//
// Every register value lives in Accel.cpp and comes from §5. The bus is shared
// with the MFRC522 NFC reader, which §5 warns "can wedge it on droopy battery
// power: use bounded I2C timeouts and retry — never wait forever". So this
// class treats a failed read as an ordinary outcome it reports, never as a
// reason to block the main loop.

#pragma once

#include <Arduino.h>

namespace badge {

struct AccelReading {
  int16_t x_mg = 0;
  int16_t y_mg = 0;
  int16_t z_mg = 0;
  bool valid   = false;
};

class Accel {
 public:
  // Brings up the I2C bus and configures the sensor. Returns false if
  // WHO_AM_I does not answer 0x11 — usually wiring or address, occasionally
  // the NFC chip holding the bus down.
  bool begin();

  bool present() const { return present_; }

  // Non-blocking. Returns valid=false when no new sample is ready or the bus
  // misbehaved, so it is safe to call every loop iteration.
  AccelReading read();

  // Failed transfers since boot. If this climbs steadily, suspect power droop
  // or NFC bus contention before suspecting your code.
  uint32_t errorCount() const { return errors_; }

 private:
  bool writeReg(uint8_t reg, uint8_t value);
  bool readRegs(uint8_t reg, uint8_t* out, uint8_t count);

  bool present_    = false;
  uint32_t errors_ = 0;
};

}  // namespace badge
