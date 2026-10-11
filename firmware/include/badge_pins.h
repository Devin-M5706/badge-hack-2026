// badge_pins.h — HTN 2026 hacker badge (ESP32-C3-MINI-1-N4) pin map.
//
// SOURCE OF TRUTH. Every value here is transcribed from the official
// "Custom firmware + HAL guide (2026 Hacker Badge)", §2/§3/§5/§6.
// Do not substitute values from generic ESP32 tutorials — this board does not
// use the usual default SPI/I2C pins.
//
// If you change a number here, you are changing it everywhere. That is the
// point.

#pragma once

#include <stdint.h>

// ── Display: ST7789, 320x240, RGB565, SPI2 @ 40 MHz, SPI mode 0 (§4) ────────
constexpr int PIN_LCD_MOSI = 10;
constexpr int PIN_LCD_CLK  = 1;
constexpr int PIN_LCD_CS   = 2;
constexpr int PIN_LCD_DC   = 0;
constexpr int PIN_LCD_RST  = 4;
constexpr int PIN_LCD_MISO = -1;  // write-only panel; no read line wired

constexpr uint32_t LCD_SPI_HZ = 40000000;
constexpr int LCD_W           = 320;
constexpr int LCD_H           = 240;

// ── Shared I2C bus: accelerometer + NFC, 400 kHz (§5, §7) ──────────────────
constexpr int PIN_I2C_SDA = 5;
constexpr int PIN_I2C_SCL = 6;
constexpr uint32_t I2C_HZ = 400000;

constexpr uint8_t ADDR_ACCEL = 0x19;  // SC7A20HTR
constexpr uint8_t ADDR_NFC   = 0x26;  // MFRC522

// The NFC chip shares this bus and can wedge it on droopy AA power (§5).
// Never block forever on a transfer — every I2C op gets a bounded timeout.
constexpr uint32_t I2C_TIMEOUT_MS = 50;

// ── Buttons: 8 inputs on a 74HC165 shift register + dedicated Start (§3) ───
constexpr int PIN_HC165_DATA = 7;   // serial out of the shift register
constexpr int PIN_HC165_LOAD = 20;  // pulse LOW to latch all 8 inputs
constexpr int PIN_HC165_CLK  = 21;

// Start has its own GPIO and is NOT on the shift register.
// GPIO9 is also the boot strapping pin: holding Start while plugging in USB
// enters download mode. That is the documented flashing procedure (§1.4),
// not a fault — but it does mean "hold Start at power-on" is never a usable
// input gesture in your app.
constexpr int PIN_START = 9;

// ── LEDs: 6x WS2812B-2020, single data line, GRB order (§6) ────────────────
constexpr int PIN_LED_DIN = 3;
constexpr int LED_COUNT   = 6;

// 6 LEDs at full white can brown out the board on AA power (§6). This cap is
// enforced in Leds::begin(); raise it only while on USB and only knowingly.
constexpr uint8_t LED_MAX_BRIGHTNESS = 40;  // of 255
