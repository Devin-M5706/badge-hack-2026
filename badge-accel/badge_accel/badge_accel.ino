// HTN 2026 hacker badge (ESP32-C3-MINI-1-N4) - accelerometer + live bump chart.
//
// Reads the SC7A20 at 100 Hz, streams samples as CSV over USB-Serial-JTAG, and
// draws a real-time sweep chart of acceleration magnitude on the ST7789 so
// bumps are visible as they happen. No NFC, no BLE, no buttons.
//
// Pin/register values come from the official badge HAL guide:
// https://badge.hackthenorth.com/custom-flash
//
// Arduino IDE settings that matter:
//   Board: "ESP32C3 Dev Module"
//   USB CDC On Boot: ENABLED      <-- without this, Serial goes nowhere

#include <Wire.h>
#include <SPI.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_NeoPixel.h>
#include "logo.h"

// ---- badge pin map ----
static const int PIN_SDA = 5;
static const int PIN_SCL = 6;
static const uint32_t I2C_HZ = 400000;

// 6x WS2812B-2020 around the board edge, GRB, single data line (HAL guide
// section 6). Brightness is capped hard: the guide warns that six LEDs at full
// white can brown out this board on AA power, and a brown-out at the instant of
// an impact looks exactly like a crash in the one moment you need the I2C read.
static const int PIN_LEDS     = 3;
static const int LED_COUNT    = 6;
static const uint8_t LED_CAP  = 48;   // never raise this on battery

static const int PIN_LCD_MOSI = 10;
static const int PIN_LCD_CLK  = 1;
static const int PIN_LCD_CS   = 2;
static const int PIN_LCD_DC   = 0;
static const int PIN_LCD_RST  = 4;
static const uint32_t LCD_HZ  = 40000000;

// The HAL guide calls for swap_xy + mirror, which maps to Adafruit rotation 1,
// but that lands upside down on this unit. 3 is the same turned 180 degrees.
static const uint8_t LCD_ROTATION = 3;

// ---- SC7A20 ----
static const uint8_t ACCEL_ADDR   = 0x19;
static const uint8_t REG_WHO_AM_I = 0x0F;  // expect 0x11
static const uint8_t REG_CTRL1    = 0x20;  // 0x57 = 100 Hz, X/Y/Z enabled
static const uint8_t REG_CTRL4    = 0x23;  // 0x80 = block data update, +/-2 g
static const uint8_t REG_STATUS   = 0x27;  // bit 3 = ZYXDA
static const uint8_t REG_OUT_X_L  = 0x28;  // | 0x80 for auto-increment

static const uint8_t WHO_AM_I_EXPECTED = 0x11;
static const uint8_t STATUS_ZYXDA      = 0x08;
static const uint32_t I2C_TIMEOUT_MS   = 20;

// ---- colours (RGB565) ----
static const uint16_t COL_BG     = 0x0000;  // black
static const uint16_t COL_TEXT   = 0xFFFF;  // white
static const uint16_t COL_TRACE  = 0x07FF;  // cyan
static const uint16_t COL_BUMP   = 0xF800;  // red
static const uint16_t COL_THRESH = 0xFD20;  // orange
static const uint16_t COL_GRID   = 0x2124;  // dark grey

// ---- layout (320x240 landscape) ----
static const int16_t LOGO_X   = 6;
static const int16_t LOGO_Y   = 6;
static const int16_t TEXT_X   = 110;
static const int16_t TITLE_Y  = 8;
static const int16_t READ_Y   = 38;
static const int16_t BUMP_Y   = 58;
// The plot is inset from the left to leave a gutter for the mg scale, and
// shortened at the bottom to leave a strip for the time ticks. Labels live
// OUTSIDE the plot rectangle on purpose: the sweep erases four columns ahead
// of the cursor, so anything drawn inside would be wiped within one pass.
static const int16_t AXIS_GUTTER = 27;   // room for 4 digits at text size 1
static const int16_t CHART_X  = AXIS_GUTTER + 3;
static const int16_t CHART_Y  = 84;
static const int16_t CHART_W  = 317 - CHART_X;
static const int16_t CHART_H  = 136;     // was 150; the rest is the time axis

// Sample rate the firmware actually achieves, measured at 97.8 Hz over 9.2 s.
// Used only to turn chart columns into seconds for the time axis, so a label
// that says "1s" means one second of real history.
static const int32_t SAMPLE_HZ = 98;

// Full scale for the chart. The sensor is +/-2 g per axis, so a hard bump
// saturates well above this; the trace simply clips at the top.
static const int32_t MG_FULL_SCALE = 3000;
static const int32_t MG_REST       = 1000;  // 1 g reference line
static const int32_t MG_BUMP       = 1600;  // bump threshold - tune to taste
static const uint32_t BUMP_HOLD_MS = 600;   // how long the BUMP flag stays lit
static const uint32_t BUMP_GAP_MS  = 250;   // debounce between reported bumps
static const uint32_t READOUT_MS   = 200;   // numeric refresh rate

static const char BANNER[] = "compiled-15";

Adafruit_ST7789 tft(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
Adafruit_NeoPixel leds(LED_COUNT, PIN_LEDS, NEO_GRB + NEO_KHZ800);

static uint32_t sample_index = 0;
static int16_t chart_col = 0;
static int16_t prev_y = -1;
static int16_t rest_y = 0;
static int16_t thresh_y = 0;
static uint32_t bump_until_ms = 0;
static uint32_t last_bump_ms = 0;
static uint32_t last_readout_ms = 0;
static bool bump_shown = false;
static int32_t peak_mg = 0;        // running peak, shown next to the live value
static int32_t last_bump_mg = 0;   // magnitude of the knock that tripped BUMP
static uint32_t led_until_ms = 0;  // non-blocking flash deadline
static bool leds_lit = false;

// ---------------- accelerometer (verified working) ----------------

static bool readReg(uint8_t reg, uint8_t *out) {
  Wire.beginTransmission(ACCEL_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)ACCEL_ADDR, (uint8_t)1) != 1) return false;
  *out = Wire.read();
  return true;
}

static bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ACCEL_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

// Each axis is 12-bit left-justified in an int16, so >>4 gives counts, and at
// +/-2 g one count is 1 mg.
static bool readAccelMilliG(int16_t *ax, int16_t *ay, int16_t *az) {
  Wire.beginTransmission(ACCEL_ADDR);
  Wire.write(REG_OUT_X_L | 0x80);
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((uint8_t)ACCEL_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  *ax = (int16_t)((uint16_t)b[1] << 8 | b[0]) >> 4;
  *ay = (int16_t)((uint16_t)b[3] << 8 | b[2]) >> 4;
  *az = (int16_t)((uint16_t)b[5] << 8 | b[4]) >> 4;
  return true;
}

static bool accelBringUp() {
  uint8_t who = 0;
  if (!readReg(REG_WHO_AM_I, &who)) {
    Serial.println("# ERROR: no I2C response at 0x19");
    return false;
  }
  if (who != WHO_AM_I_EXPECTED) {
    Serial.printf("# ERROR: WHO_AM_I = 0x%02X, expected 0x%02X\n",
                  who, WHO_AM_I_EXPECTED);
    return false;
  }
  if (!writeReg(REG_CTRL1, 0x57) || !writeReg(REG_CTRL4, 0x80)) {
    Serial.println("# ERROR: could not configure CTRL_REG1/CTRL_REG4");
    return false;
  }
  Serial.println("# SC7A20 ok: 100 Hz, +/-2 g, units = mg");
  return true;
}

// ---------------- impact LEDs ----------------

// Colour carries the severity, so the lights say more than "something hit it":
// amber at the threshold, red for a hard strike. The scale runs from the bump
// threshold to chart full scale, which is the same range the trace uses, so the
// lights and the chart agree about what counts as hard.
static void ledsImpact(int32_t mg) {
  int32_t span = MG_FULL_SCALE - MG_BUMP;
  int32_t over = mg - MG_BUMP;
  if (over < 0) over = 0;
  if (span < 1) span = 1;
  if (over > span) over = span;

  const uint8_t r = (uint8_t)(LED_CAP / 2 + (LED_CAP / 2) * over / span);
  const uint8_t g = (uint8_t)((LED_CAP / 2) * (span - over) / span);

  for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(r, g, 0));
  leds.show();
  leds_lit = true;
}

static void ledsService(uint32_t now) {
  if (leds_lit && now >= led_until_ms) {
    leds.clear();
    leds.show();
    leds_lit = false;
  }
}

// ---------------- display ----------------

static void drawAxes();
static void gutterLabel(int32_t mg, int16_t y, uint16_t colour);

static int16_t valueToY(int32_t mg) {
  if (mg < 0) mg = 0;
  if (mg > MG_FULL_SCALE) mg = MG_FULL_SCALE;
  int32_t span = CHART_H - 1;
  return (int16_t)(CHART_Y + span - (mg * span / MG_FULL_SCALE));
}

static void drawStaticUi() {
  tft.fillScreen(COL_BG);
  tft.drawRGBBitmap(LOGO_X, LOGO_Y, LOGO_RGB565, LOGO_W, LOGO_H);

  tft.setTextColor(COL_TEXT);
  tft.setTextSize(3);
  tft.setCursor(TEXT_X, TITLE_Y);
  tft.print(BANNER);

  rest_y = valueToY(MG_REST);
  thresh_y = valueToY(MG_BUMP);

  tft.drawRect(CHART_X - 1, CHART_Y - 1, CHART_W + 2, CHART_H + 2, COL_GRID);
  tft.drawFastHLine(CHART_X, rest_y, CHART_W, COL_GRID);
  tft.drawFastHLine(CHART_X, thresh_y, CHART_W, COL_THRESH);

  drawAxes();
}

// Right-align a number in the gutter so the digits line up down the scale
// instead of drifting with the value's width.
static void gutterLabel(int32_t mg, int16_t y, uint16_t colour) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%ld", (long)mg);
  const int16_t w = (int16_t)strlen(buf) * 6;          // size 1 = 6 px/char
  int16_t ty = y - 3;                                  // centre on the tick
  if (ty < CHART_Y - 3) ty = CHART_Y - 3;
  if (ty > CHART_Y + CHART_H - 5) ty = CHART_Y + CHART_H - 5;
  tft.setTextSize(1);
  tft.setTextColor(colour);
  tft.setCursor(AXIS_GUTTER - w, ty);
  tft.print(buf);
  tft.drawFastHLine(AXIS_GUTTER + 1, y, 2, colour);    // tick into the plot
}

// Both axes, drawn once. Every labelled value is one the chart can actually
// reach: 0 and 3000 are the clip limits, 1000 is the 1 g rest line the trace
// sits on, 1600 is the bump threshold the trace changes colour at.
static void drawAxes() {
  const int16_t bottom = CHART_Y + CHART_H - 1;

  // --- Y: milligravities ---
  tft.setTextSize(1);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(0, CHART_Y - 13);
  tft.print("mg");

  gutterLabel(MG_FULL_SCALE, valueToY(MG_FULL_SCALE), COL_TEXT);
  gutterLabel(MG_BUMP,       thresh_y,                COL_THRESH);
  gutterLabel(MG_REST,       rest_y,                  COL_GRID);
  gutterLabel(0,             bottom,                  COL_TEXT);

  // Name the two reference lines, not just their values -- a number alone
  // does not say which line is the threshold.
  tft.setTextSize(1);
  tft.setTextColor(COL_THRESH);
  tft.setCursor(CHART_X + 2, thresh_y - 9);
  tft.print("bump");
  tft.setTextColor(COL_GRID);
  tft.setCursor(CHART_X + 2, rest_y + 2);
  tft.print("1g rest");

  // --- X: seconds since this sweep started ---
  // The trace is a wrapping sweep, not a scrolling window, so these ticks are
  // "time since the cursor last wrapped" -- which is what the x position
  // genuinely encodes. Labelling them "-1s, -2s" would be wrong.
  tft.setTextColor(COL_TEXT);
  for (int32_t s = 0; s * SAMPLE_HZ < CHART_W; s++) {
    const int16_t x = (int16_t)(CHART_X + s * SAMPLE_HZ);
    tft.drawFastVLine(x, bottom + 2, 3, COL_GRID);
    char buf[6];
    snprintf(buf, sizeof(buf), "%lds", (long)s);
    tft.setCursor(x - 2, bottom + 7);
    tft.print(buf);
  }

  const int16_t secs10 = (int16_t)((CHART_W * 10) / SAMPLE_HZ);
  char cap[40];
  snprintf(cap, sizeof(cap), "sweep %d.%ds @ %ldHz",
           secs10 / 10, secs10 % 10, (long)SAMPLE_HZ);
  const int16_t cw = (int16_t)strlen(cap) * 6;
  tft.setTextColor(COL_GRID);
  tft.setCursor(CHART_X + CHART_W - cw, bottom + 7);
  tft.print(cap);
}

// One sweep step: clear a short gap ahead of the cursor, then join the last
// sample to this one. Only a few columns are touched per sample, so the chart
// never competes with the 100 Hz polling.
static void chartStep(int32_t mg) {
  for (int16_t i = 0; i < 4; i++) {
    int16_t cx = CHART_X + ((chart_col + i) % CHART_W);
    tft.drawFastVLine(cx, CHART_Y, CHART_H, COL_BG);
    tft.drawPixel(cx, rest_y, COL_GRID);
    tft.drawPixel(cx, thresh_y, COL_THRESH);
  }

  int16_t x = CHART_X + chart_col;
  int16_t y = valueToY(mg);
  uint16_t colour = (mg >= MG_BUMP) ? COL_BUMP : COL_TRACE;
  if (prev_y < 0) {
    tft.drawPixel(x, y, colour);
  } else {
    tft.drawLine(x - 1, prev_y, x, y, colour);
  }

  prev_y = y;
  chart_col++;
  if (chart_col >= CHART_W) {
    chart_col = 0;
    prev_y = -1;
  }
}

static void updateReadout(int32_t mg, bool bumping) {
  // Live magnitude and the running peak, both named. An unlabelled number on
  // a demo screen is a number nobody can quote back at you.
  if (mg > peak_mg) peak_mg = mg;

  tft.fillRect(TEXT_X, READ_Y, 320 - TEXT_X, 16, COL_BG);
  tft.setTextSize(1);
  tft.setTextColor(COL_GRID);
  tft.setCursor(TEXT_X, READ_Y);
  tft.print("now");
  tft.setCursor(TEXT_X + 108, READ_Y);
  tft.print("peak");

  tft.setTextSize(2);
  tft.setTextColor(COL_TRACE);
  tft.setCursor(TEXT_X + 24, READ_Y);
  tft.print(mg);
  tft.setTextSize(1);
  tft.setCursor(TEXT_X + 24 + 48, READ_Y + 7);
  tft.print("mg");

  tft.setTextSize(2);
  tft.setTextColor(COL_TEXT);
  tft.setCursor(TEXT_X + 136, READ_Y);
  tft.print(peak_mg);

  if (bumping != bump_shown) {
    tft.fillRect(TEXT_X, BUMP_Y, 320 - TEXT_X, 16, COL_BG);
    if (bumping) {
      // The magnitude that tripped it, not just the word -- so the number on
      // screen is traceable to the knock you just made.
      tft.setTextSize(2);
      tft.setTextColor(COL_BUMP);
      tft.setCursor(TEXT_X, BUMP_Y);
      tft.print("BUMP ");
      tft.print(last_bump_mg);
      tft.setTextSize(1);
      tft.setCursor(TEXT_X + 10 * 12 + 4, BUMP_Y + 7);
      tft.print("mg");
    }
    bump_shown = bumping;
  }
}

// ---------------- main ----------------

void setup() {
  Serial.begin(115200);

  // Critical on native USB: without this, every write blocks waiting for a
  // host to drain the CDC FIFO. With nothing attached the loop collapses to
  // under 1 Hz and the chart appears frozen. Zero means never wait - output is
  // simply discarded when no one is listening, and the badge runs standalone.
  Serial.setTxTimeoutMs(0);

  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Wire.begin(PIN_SDA, PIN_SCL, I2C_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);

  SPI.begin(PIN_LCD_CLK, -1, PIN_LCD_MOSI, PIN_LCD_CS);
  tft.init(240, 320);
  tft.setSPISpeed(LCD_HZ);
  tft.invertDisplay(true);
  tft.setRotation(LCD_ROTATION);

  Serial.println();
  Serial.println("# badge accelerometer stream");
  leds.begin();
  leds.clear();
  leds.show();

  drawStaticUi();
  Serial.print("# display: ");
  Serial.print(tft.width());
  Serial.print("x");
  Serial.print(tft.height());
  Serial.print(", rotation ");
  Serial.print(LCD_ROTATION);
  Serial.print(", logo ");
  Serial.print(LOGO_W);
  Serial.print("x");
  Serial.println(LOGO_H);
  Serial.print("# chart: 0-");
  Serial.print(MG_FULL_SCALE);
  Serial.print(" mg, bump threshold ");
  Serial.print(MG_BUMP);
  Serial.println(" mg");

  while (!accelBringUp()) {
    delay(1000);
  }

  Serial.println("n,t_ms,ax_mg,ay_mg,az_mg");
}

void loop() {
  uint8_t status = 0;
  if (!readReg(REG_STATUS, &status)) {
    delay(5);
    return;
  }
  if (!(status & STATUS_ZYXDA)) {
    delay(1);
    return;
  }

  int16_t ax, ay, az;
  if (!readAccelMilliG(&ax, &ay, &az)) {
    delay(5);
    return;
  }

  uint32_t now = millis();
  Serial.printf("%lu,%lu,%d,%d,%d\n",
                (unsigned long)sample_index++, (unsigned long)now, ax, ay, az);

  int32_t mg = (int32_t)lroundf(sqrtf((float)ax * ax +
                                      (float)ay * ay +
                                      (float)az * az));

  if (mg >= MG_BUMP && now - last_bump_ms >= BUMP_GAP_MS) {
    last_bump_ms = now;
    last_bump_mg = mg;          // the magnitude the on-screen BUMP label shows
    bump_until_ms = now + BUMP_HOLD_MS;
    Serial.printf("# BUMP n=%lu mag=%ld mg\n",
                  (unsigned long)(sample_index - 1), (long)mg);
  }

  chartStep(mg);
  ledsService(now);

  if (now - last_readout_ms >= READOUT_MS) {
    last_readout_ms = now;
    updateReadout(mg, now < bump_until_ms);
  }
}
