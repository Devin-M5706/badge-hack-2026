#include "Console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Accel.h"
#include "Buttons.h"
#include "Display.h"
#include "Leds.h"
#include "badge_pins.h"

namespace badge {
namespace {

constexpr size_t kMaxWrite = 200;  // under the 256 B cap from §9

// Returns the next whitespace-delimited token and advances *cursor past it.
char* nextToken(char** cursor) {
  if (cursor == nullptr || *cursor == nullptr) return nullptr;
  char* p = *cursor;
  while (*p == ' ' || *p == '\t') ++p;
  if (*p == '\0') {
    *cursor = p;
    return nullptr;
  }
  char* start = p;
  while (*p != '\0' && *p != ' ' && *p != '\t') ++p;
  if (*p != '\0') {
    *p = '\0';
    ++p;
  }
  *cursor = p;
  return start;
}

long tokenToLong(const char* token, long fallback) {
  if (token == nullptr || *token == '\0') return fallback;
  char* end = nullptr;
  const long value = strtol(token, &end, 10);
  return (end != nullptr && *end == '\0') ? value : fallback;
}

}  // namespace

void Console::begin(Buttons* buttons, Leds* leds, Accel* accel,
                    Display* display) {
  buttons_ = buttons;
  leds_    = leds;
  accel_   = accel;
  display_ = display;
  len_     = 0;

  Serial.println();
  Serial.println(F("badge console ready. type 'help'."));
}

void Console::writeChunked(const char* text) {
  if (text == nullptr) return;
  size_t remaining = strlen(text);
  while (remaining > 0) {
    const size_t n = remaining > kMaxWrite ? kMaxWrite : remaining;
    Serial.write(reinterpret_cast<const uint8_t*>(text), n);
    text += n;
    remaining -= n;
  }
}

void Console::poll() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;

    // §1.5 notes the console line ending is a bare \r, but monitors differ, so
    // accept either and ignore empty lines rather than guessing.
    if (c == '\r' || c == '\n') {
      if (len_ > 0) {
        buf_[len_] = '\0';
        dispatch(buf_);
        len_ = 0;
      }
      continue;
    }

    if (len_ + 1 < sizeof(buf_)) {
      buf_[len_++] = static_cast<char>(c);
    } else {
      // Overlong line: drop it and say so, rather than silently truncating
      // into a command that does something different from what was typed.
      len_ = 0;
      Serial.println(F("! line too long, discarded"));
    }
  }
}

void Console::printHelp() {
  writeChunked(
      "commands:\r\n"
      "  help            this list\r\n"
      "  state           buttons, accel, uptime, heap\r\n"
      "  btn <name> [ms] inject a press (a b home down left right up aux1 "
      "start)\r\n"
      "  led <i> <r> <g> <b>  set one LED (i 0..5, values 0..255)\r\n"
      "  leds off        all LEDs off\r\n"
      "  rot <0..3>      display rotation, if the image looks mirrored\r\n");
}

void Console::printState() {
  char out[200];

  if (buttons_ != nullptr) {
    // Show both the decoded names and the pre-inversion raw byte: when a
    // button seems dead, the raw byte tells you whether the wire is moving at
    // all, which separates a wiring fault from a decode bug.
    char names[96];
    size_t used = 0;
    names[0]    = '\0';
    for (uint8_t i = 0; i < BTN_COUNT; ++i) {
      if (!buttons_->isDown(i)) continue;
      const int n = snprintf(names + used, sizeof(names) - used, "%s%s",
                             used > 0 ? "," : "", buttonName(i));
      if (n <= 0 || static_cast<size_t>(n) >= sizeof(names) - used) break;
      used += static_cast<size_t>(n);
    }
    snprintf(out, sizeof(out), "buttons: [%s] raw=0x%02X\r\n",
             used > 0 ? names : "none", buttons_->lastRawByte());
    writeChunked(out);
  }

  if (accel_ != nullptr) {
    if (accel_->present()) {
      const AccelReading r = accel_->read();
      if (r.valid) {
        snprintf(out, sizeof(out), "accel: x=%d y=%d z=%d mg (errs=%lu)\r\n",
                 r.x_mg, r.y_mg, r.z_mg,
                 static_cast<unsigned long>(accel_->errorCount()));
      } else {
        snprintf(out, sizeof(out), "accel: no new sample (errs=%lu)\r\n",
                 static_cast<unsigned long>(accel_->errorCount()));
      }
    } else {
      snprintf(out, sizeof(out), "accel: ABSENT (WHO_AM_I != 0x11)\r\n");
    }
    writeChunked(out);
  }

  snprintf(out, sizeof(out), "uptime: %lu ms   free heap: %lu B\r\n",
           static_cast<unsigned long>(millis()),
           static_cast<unsigned long>(ESP.getFreeHeap()));
  writeChunked(out);
}

void Console::dispatch(char* line) {
  char* cursor = line;
  const char* cmd = nextToken(&cursor);
  if (cmd == nullptr) return;

  if (strcmp(cmd, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(cmd, "state") == 0) {
    printState();
    return;
  }

  if (strcmp(cmd, "btn") == 0) {
    const char* name = nextToken(&cursor);
    const int button = buttonFromName(name);
    if (button < 0) {
      Serial.println(F("! unknown button"));
      return;
    }
    if (buttons_ == nullptr) {
      Serial.println(F("! buttons unavailable"));
      return;
    }
    const long ms = tokenToLong(nextToken(&cursor), 60);
    buttons_->inject(static_cast<uint8_t>(button),
                     static_cast<uint32_t>(ms > 0 ? ms : 60));
    Serial.print(F("injected "));
    Serial.println(buttonName(static_cast<uint8_t>(button)));
    return;
  }

  if (strcmp(cmd, "led") == 0) {
    if (leds_ == nullptr) {
      Serial.println(F("! leds unavailable"));
      return;
    }
    const long index = tokenToLong(nextToken(&cursor), -1);
    if (index < 0 || index >= LED_COUNT) {
      Serial.println(F("! led index must be 0..5"));
      return;
    }
    const long r = tokenToLong(nextToken(&cursor), 0);
    const long g = tokenToLong(nextToken(&cursor), 0);
    const long b = tokenToLong(nextToken(&cursor), 0);
    leds_->set(static_cast<uint8_t>(index), static_cast<uint8_t>(r),
               static_cast<uint8_t>(g), static_cast<uint8_t>(b));
    leds_->show();
    Serial.println(F("ok"));
    return;
  }

  if (strcmp(cmd, "leds") == 0) {
    const char* arg = nextToken(&cursor);
    if (leds_ != nullptr && arg != nullptr && strcmp(arg, "off") == 0) {
      leds_->clear();
      leds_->show();
      Serial.println(F("ok"));
      return;
    }
    Serial.println(F("! usage: leds off"));
    return;
  }

  if (strcmp(cmd, "rot") == 0) {
    if (display_ == nullptr) {
      Serial.println(F("! display unavailable"));
      return;
    }
    const long rotation = tokenToLong(nextToken(&cursor), -1);
    if (rotation < 0 || rotation > 3) {
      Serial.println(F("! rotation must be 0..3"));
      return;
    }
    display_->setRotationVariant(static_cast<uint8_t>(rotation));
    Serial.println(F("ok"));
    return;
  }

  Serial.print(F("! unknown command: "));
  Serial.println(cmd);
}

}  // namespace badge
