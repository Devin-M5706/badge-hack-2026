// BumpDetector.h — the hardware half of knock detection.
//
// Splits cleanly from the algorithm on purpose: bump::Detector (lib/BumpLogic)
// is pure integer logic with no Arduino headers, so CI exercises every
// discriminator against synthetic waveforms. This class is the part that
// cannot be tested on a laptop — the sampling task, the queue, the I2C
// ownership and the NVS persistence.
//
// WHY A DEDICATED TASK. The detector needs an even 400 Hz. The main loop polls
// buttons every 10 ms and a full-screen ST7789 blit is ~31 ms of blocking SPI
// on a single-core part, so sampling from loop() would drop a third of the
// samples the moment anything is drawn. The task runs above loopTask and
// reports its own achieved rate, so starvation is a number you read rather
// than a thing you suspect.
//
// I2C OWNERSHIP. While this is running it is the only user of the
// accelerometer. The console reads lastReading() — a cached copy — instead of
// touching the bus, which removes the race entirely. Around any NFC
// transaction, call pause() / resume(): the bus is shared with the MFRC522 and
// §7 says to suspend accelerometer polling while the field is up. resume()
// re-seeds gravity, because the step across the gap would otherwise fire a
// false bump on every single scan.

#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Accel.h"
#include "bump_logic.h"

namespace badge {

struct BumpEvent {
  uint32_t t_ms    = 0;  // millis() at the DECISION sample, which is the peak
                         // plus the ringdown window (~60 ms), not the impact
  int32_t peak_mg  = 0;
  uint8_t axis     = 0;  // bump::kAxisX/Y/Z. Z => face or flat-on-desk knock
  uint8_t flags    = 0;  // bump::kFlag* bitmask
  uint32_t seq     = 0;  // counts detections; a gap here means a dropped event
};

class BumpDetector {
 public:
  // `accel` must already have begun() successfully and must not be read by
  // anyone else afterwards. Returns false if the task or queue cannot be
  // created, or if the sensor is absent.
  bool begin(Accel* accel);

  bool running() const { return task_ != nullptr; }

  // Non-blocking drain. Call every loop iteration; returns true per event.
  bool poll(BumpEvent* out);

  // ── training capture ────────────────────────────────────────────────────
  // With capture on, every armed candidate — accepted or rejected — is queued
  // for the console to print as a CSV row, stamped with whatever label is
  // currently set. You knock thirty times with the label "knock", slam the
  // table thirty times with "slam", walk for a minute with "walk", and the
  // result is a labelled dataset off a real badge, which is the one thing no
  // public dataset can supply.
  void captureOn(bool on) { capture_ = on; }
  bool capturing() const { return capture_; }
  void setLabel(const char* label);
  const char* label() const { return label_; }
  bool pollCandidate(bump::Candidate* out, char* label_out, size_t label_cap);
  uint32_t candidatesSeen() const { return cand_seen_; }
  uint32_t candidatesDropped() const { return cand_drop_; }

  // Around NFC. Safe from any task, idempotent.
  void pause();
  void resume();

  // Live-tunable. Word-sized fields, written from the console task and read by
  // the sampler: a torn update costs one misjudged sample, which is why there
  // is no mutex here.
  bump::Config& config() { return logic_.config(); }
  const bump::Stats& logicStats() const { return logic_.stats(); }

  // The learned decision rule, surfaced for the console so both rules can be
  // measured on the same hardware.
  const bump::Model& config_model() const { return logic_.model(); }
  void setUseModel(bool on) { logic_.useModel(on); }
  bool usingModel() const { return logic_.usingModel(); }
  void clearStats();

  // Cached, no bus access — for console readouts.
  AccelReading lastReading() const;

  // ── health: proof the sampler is not being starved ──────────────────────
  uint32_t sampleRateHz() const;
  uint32_t worstGapUs() const { return worst_gap_us_; }
  uint32_t lateCount() const { return late_; }
  uint32_t emptyPolls() const { return empty_; }  // polls with no new sample;
                                                  // ~20% is healthy, ~0% means
                                                  // we are slower than the ODR
  uint32_t drops() const { return drops_; }

  // ── config persistence ──────────────────────────────────────────────────
  // Every reflash needs a manual hold-Start-while-plugging cycle, and a
  // brown-out from the LEDs on AA cells reverts RAM silently. Without this you
  // tune twice.
  bool saveConfig();
  bool loadConfig();
  void resetConfig();

 private:
  static void trampoline(void* self);
  void loop();

  struct CandRow {
    bump::Candidate c;
    char label[12];
  };

  Accel* accel_            = nullptr;
  bump::Detector logic_;
  QueueHandle_t queue_     = nullptr;
  QueueHandle_t cand_q_    = nullptr;
  TaskHandle_t task_       = nullptr;
  volatile bool paused_    = false;
  volatile bool capture_   = false;
  char label_[12]          = "unlabelled";
  uint32_t cand_seen_      = 0;
  uint32_t cand_drop_      = 0;

  AccelReading last_{};
  uint32_t samples_        = 0;
  uint32_t empty_          = 0;
  uint32_t late_           = 0;
  uint32_t worst_gap_us_   = 0;
  uint32_t drops_          = 0;
  uint32_t started_ms_     = 0;
};

}  // namespace badge
