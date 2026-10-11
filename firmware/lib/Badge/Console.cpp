#include "Console.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Accel.h"
#include "BumpDetector.h"
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
  char* end        = nullptr;
  const long value = strtol(token, &end, 10);
  return (end != nullptr && *end == '\0') ? value : fallback;
}

}  // namespace

void Console::begin(Buttons* buttons, Leds* leds, Accel* accel,
                    Display* display, BumpDetector* bump) {
  buttons_ = buttons;
  leds_    = leds;
  accel_   = accel;
  display_ = display;
  bump_    = bump;
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
  writeChunked(
      "knock detector:\r\n"
      "  bump            health line + current tuning (read this every "
      "pass)\r\n"
      "  bump flip       1g check: one axis ~+/-1000 mg at rest\r\n"
      "  bump thr <mg>   peak threshold        bump qgate <mg>  quiet gate\r\n"
      "  bump quiet <ms> quiet credit needed   bump refr <ms>   refractory\r\n"
      "  bump win <ms>   ringdown window       bump hot <n>     max hot "
      "samples\r\n"
      "  bump pen <n>    blip penalty          bump agit <0|1>  agitation "
      "veto\r\n"
      "  bump agit <n>   agitation level (n>1) bump "
      "save|load|defaults|zero\r\n");
  writeChunked(
      "training capture:\r\n"
      "  bump label <name>   tag rows: knock | slam | walk | desk ...\r\n"
      "  bump cap [off]      stream one CSV row per armed candidate\r\n"
      "  bump model [off]    switch the final accept to the learned rule\r\n");
}

// One line, under the 256-byte USB write limit, printed once per tuning pass.
//
// Read `hz` and `gap` first. hz well under 400 or a gap in the tens of
// thousands of microseconds means the sampler is being starved by the display
// or the LEDs, and no amount of threshold tuning will fix that. `empty` near
// zero is the subtle one: it means we are polling slower than the sensor
// produces data and silently losing samples.
void Console::printBump() {
  if (bump_ == nullptr) {
    Serial.println(F("! bump detector unavailable"));
    return;
  }
  const bump::Stats& s  = bump_->logicStats();
  const bump::Config& c = bump_->config();
  const AccelReading r  = bump_->lastReading();

  char out[256];
  snprintf(out, sizeof(out),
           "hz=%lu gap=%lu late=%lu empty=%lu | ev=%lu drop=%lu rq=%lu ra=%lu "
           "rd=%lu | mag=%ld pk=%ld qr=%ld ag=%ld | xyz %d %d %d\r\n",
           static_cast<unsigned long>(bump_->sampleRateHz()),
           static_cast<unsigned long>(bump_->worstGapUs()),
           static_cast<unsigned long>(bump_->lateCount()),
           static_cast<unsigned long>(bump_->emptyPolls()),
           static_cast<unsigned long>(s.events),
           static_cast<unsigned long>(bump_->drops()),
           static_cast<unsigned long>(s.rej_quiet),
           static_cast<unsigned long>(s.rej_agit),
           static_cast<unsigned long>(s.rej_decay),
           static_cast<long>(s.mag_now), static_cast<long>(s.mag_max),
           static_cast<long>(s.qrun), static_cast<long>(s.agit), r.x_mg, r.y_mg,
           r.z_mg);
  writeChunked(out);

  // Print every tunable, including the three that are not settable from here
  // (dshift, dq_need, rlock). A knob you cannot see is a knob you will not
  // think of when the numbers look wrong.
  snprintf(out, sizeof(out),
           "thr=%ld qgate=%ld quiet=%ldms refr=%ldms win=%ldms hot=%ld/%ld "
           "pen=%ld agit=%ld lvl=%ld | dshift=%ld dq=%ld rlock=%ldms\r\n",
           static_cast<long>(c.thr_mg), static_cast<long>(c.qgate_mg),
           static_cast<long>(c.qneed * bump::kMsPerIter),
           static_cast<long>(c.refr * bump::kMsPerIter),
           static_cast<long>(c.dwin * bump::kMsPerIter),
           static_cast<long>(c.dhot_max), static_cast<long>(c.dwin),
           static_cast<long>(c.qpen), static_cast<long>(c.agit_en),
           static_cast<long>(c.agit_max), static_cast<long>(c.dshift),
           static_cast<long>(c.dq_need),
           static_cast<long>(c.rlock * bump::kMsPerIter));
  writeChunked(out);
}

void Console::bumpCommand(char** cursor) {
  if (bump_ == nullptr) {
    Serial.println(F("! bump detector unavailable"));
    return;
  }
  const char* sub = nextToken(cursor);
  if (sub == nullptr) {
    printBump();
    return;
  }

  bump::Config& c = bump_->config();

  if (strcmp(sub, "flip") == 0) {
    const AccelReading r = bump_->lastReading();
    char out[160];
    snprintf(out, sizeof(out),
             "flip x=%d y=%d z=%d mg | expect ONE axis near +/-1000 at rest; "
             "all three near zero means no data, ~250 means HR/FS did not "
             "take\r\n",
             r.x_mg, r.y_mg, r.z_mg);
    writeChunked(out);
    return;
  }
  if (strcmp(sub, "save") == 0) {
    Serial.println(bump_->saveConfig() ? F("saved") : F("! save failed"));
    return;
  }
  if (strcmp(sub, "load") == 0) {
    Serial.println(bump_->loadConfig() ? F("loaded")
                                       : F("! none stored, defaults"));
    printBump();
    return;
  }
  if (strcmp(sub, "defaults") == 0) {
    bump_->resetConfig();
    Serial.println(F("defaults (not saved -- 'bump save' to persist)"));
    return;
  }
  if (strcmp(sub, "zero") == 0) {
    bump_->clearStats();
    Serial.println(F("stats cleared"));
    return;
  }

  // ── training capture ────────────────────────────────────────────────────
  if (strcmp(sub, "cap") == 0) {
    const char* arg = nextToken(cursor);
    const bool on =
        (arg == nullptr) ? !bump_->capturing() : (strcmp(arg, "off") != 0);
    bump_->captureOn(on);
    if (on) {
      // Header first so the captured stream is a self-describing CSV.
      writeChunked(
          "cand,label,peak_mg,hot,qcons,qrun,agit,axis,clipped,accepted,by_"
          "model,score\r\n");
      Serial.print(F("capture ON, label="));
      Serial.println(bump_->label());
    } else {
      Serial.println(F("capture OFF"));
    }
    return;
  }
  if (strcmp(sub, "label") == 0) {
    const char* arg = nextToken(cursor);
    if (arg == nullptr) {
      Serial.print(F("label="));
      Serial.println(bump_->label());
      return;
    }
    bump_->setLabel(arg);
    Serial.print(F("label="));
    Serial.println(bump_->label());
    return;
  }
  if (strcmp(sub, "model") == 0) {
    const char* arg      = nextToken(cursor);
    const bump::Model& m = bump_->config_model();
    if (arg == nullptr) {
      Serial.printf(
          "model trained=%ld active=%d w=[%ld %ld %ld %ld %ld %ld] "
          "shift=%ld\r\n",
          (long)m.trained, bump_->usingModel() ? 1 : 0, (long)m.w0,
          (long)m.w_peak, (long)m.w_hot, (long)m.w_qcons, (long)m.w_qrun,
          (long)m.w_agit, (long)m.shift);
      return;
    }
    const bool on = strcmp(arg, "off") != 0;
    if (on && m.trained == 0) {
      Serial.println(
          F("! no trained model in this build -- staying on the hand-tuned "
            "rule"));
      return;
    }
    bump_->setUseModel(on);
    Serial.print(F("decision rule: "));
    Serial.println(bump_->usingModel() ? F("LEARNED") : F("hand-tuned"));
    return;
  }

  // Everything below takes a value. Durations are entered in milliseconds and
  // stored as iterations, because the logic counts samples, not time.
  const char* valueToken = nextToken(cursor);
  if (valueToken == nullptr) {
    Serial.println(F("! that setting needs a value"));
    return;
  }
  const long v = tokenToLong(valueToken, LONG_MIN);
  if (v == LONG_MIN) {
    Serial.println(F("! value must be a number"));
    return;
  }

  // Every out-of-range value here fails SILENTLY and looks exactly like a dead
  // accelerometer, which is the most expensive thing that can happen during a
  // 45-minute tuning pass. Two in particular:
  //   `win 1`   -> dwin 0, so --dn goes negative, the window decides on the
  //                very next sample, qcons is below dq_need, and EVERY knock
  //                is rejected as a ringdown.
  //   `quiet 0` -> the isolation veto is simply off, and nothing says so.
  // So clamp, and say what was clamped.
  auto atLeast = [](long value, long low, const char* what) -> int32_t {
    if (value < low) {
      Serial.printf(
          "! %s raised to %ld (below that it silently stops working)\r\n", what,
          low);
      return static_cast<int32_t>(low);
    }
    return static_cast<int32_t>(value);
  };
  const long minWinMs = static_cast<long>(c.dq_need * bump::kMsPerIter);

  if (strcmp(sub, "thr") == 0) {
    c.thr_mg = atLeast(v, 50, "thr");
  } else if (strcmp(sub, "qgate") == 0) {
    c.qgate_mg = atLeast(v, 1, "qgate");
  } else if (strcmp(sub, "quiet") == 0) {
    c.qneed = static_cast<int32_t>(atLeast(v, 0, "quiet") / bump::kMsPerIter);
    if (c.qneed == 0) Serial.println(F("! isolation veto is now OFF"));
  } else if (strcmp(sub, "refr") == 0) {
    c.refr = atLeast(v, bump::kMsPerIter, "refr") / bump::kMsPerIter;
  } else if (strcmp(sub, "win") == 0) {
    c.dwin = atLeast(v, minWinMs, "win") / bump::kMsPerIter;
  } else if (strcmp(sub, "hot") == 0) {
    // Above dwin the ringdown test can never reject anything; below 0 it can
    // never accept. Either way it stops being a discriminator.
    c.dhot_max = atLeast(v, 0, "hot");
    if (c.dhot_max > c.dwin) {
      c.dhot_max = c.dwin;
      Serial.printf("! hot capped at the window length (%ld)\r\n",
                    static_cast<long>(c.dwin));
    }
  } else if (strcmp(sub, "pen") == 0) {
    c.qpen = atLeast(v, 1, "pen");
  } else if (strcmp(sub, "agit") == 0) {
    // 0 and 1 switch the veto; anything larger sets its level, which is an
    // [S] starting point the acceptance protocol expects you to tune. Without
    // this it was only changeable by reflashing.
    if (v <= 1)
      c.agit_en = (v != 0) ? 1 : 0;
    else
      c.agit_max = static_cast<int32_t>(v);
  } else {
    Serial.print(F("! unknown bump setting: "));
    Serial.println(sub);
    return;
  }
  printBump();
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
  char* cursor    = line;
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

  if (strcmp(cmd, "bump") == 0) {
    bumpCommand(&cursor);
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
