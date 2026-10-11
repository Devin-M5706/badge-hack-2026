#include "BumpDetector.h"

#include <Preferences.h>
#include <esp_timer.h>

namespace badge {
namespace {

// 2 ms against a 400 Hz output rate: slightly oversampled on purpose, so a
// late wake-up still catches every sample the sensor produces. Roughly one
// poll in five finds nothing new, which is the `empty_` counter.
constexpr uint32_t POLL_MS = 2;

// Above the Arduino loopTask (priority 1) so a display blit or an LED refresh
// cannot starve sampling. Below the idle-critical system tasks.
constexpr UBaseType_t TASK_PRIO = 6;
constexpr uint32_t TASK_STACK   = 3072;

// 4 events is a full second of backlog at a 250 ms refractory. A consumer that
// cannot drain one second of knocks is wedged, and the right answer is to drop
// rather than deliver events the user already gave up on and knocked again
// for. Drop-NEWEST, because the oldest queued event is the one still awaiting
// feedback — and it is a single non-blocking send with no race.
constexpr UBaseType_t QUEUE_DEPTH = 4;

constexpr char NVS_NS[]  = "bump";
constexpr char NVS_KEY[] = "cfg";
constexpr char NVS_VER[] = "ver";

}  // namespace

bool BumpDetector::begin(Accel* accel) {
  if (accel == nullptr || !accel->present()) return false;
  accel_ = accel;

  loadConfig();  // falls back to defaults when nothing is stored

  queue_ = xQueueCreate(QUEUE_DEPTH, sizeof(BumpEvent));
  if (queue_ == nullptr) return false;

  // Deeper than the event queue: during a capture session candidates arrive
  // far more often than accepted knocks (every table slam is one), and losing
  // rows biases the training set toward whatever the console kept up with.
  cand_q_ = xQueueCreate(24, sizeof(CandRow));
  if (cand_q_ == nullptr) {
    vQueueDelete(queue_);
    queue_ = nullptr;
    return false;
  }

  started_ms_ = millis();
  logic_.reset();

  const BaseType_t ok = xTaskCreate(&BumpDetector::trampoline, "bump",
                                    TASK_STACK, this, TASK_PRIO, &task_);
  if (ok != pdPASS) {
    task_ = nullptr;
    vQueueDelete(queue_);
    queue_ = nullptr;
    return false;
  }
  return true;
}

void BumpDetector::trampoline(void* self) {
  static_cast<BumpDetector*>(self)->loop();
}

void BumpDetector::loop() {
  TickType_t next = xTaskGetTickCount();
  int64_t prev_us = esp_timer_get_time();

  for (;;) {
    if (paused_) {
      vTaskDelay(pdMS_TO_TICKS(10));
      // Re-anchor the deadline, or xTaskDelayUntil replays every missed 2 ms
      // tick in a burst and hammers the bus the instant we resume.
      next    = xTaskGetTickCount();
      prev_us = esp_timer_get_time();
      continue;
    }

    xTaskDelayUntil(&next, pdMS_TO_TICKS(POLL_MS));

    const AccelReading r = accel_->read();
    if (!r.valid) {
      ++empty_;
      continue;
    }

    const int64_t now_us  = esp_timer_get_time();
    const uint32_t gap_us = static_cast<uint32_t>(now_us - prev_us);
    prev_us               = now_us;
    if (gap_us > worst_gap_us_) worst_gap_us_ = gap_us;
    // A new sample should arrive every ~2.5 ms. Twice that means something
    // blocked us; a 320x240 blit shows up here as ~31000 immediately.
    if (gap_us > 5000u) ++late_;

    last_ = r;
    ++samples_;

    bump::Event ev;
    bump::Candidate cand{};
    bool have_cand = false;
    const bool fired =
        logic_.feed(r.x_mg, r.y_mg, r.z_mg, &ev, capture_ ? &cand : nullptr,
                    capture_ ? &have_cand : nullptr);

    if (have_cand && cand_q_ != nullptr) {
      ++cand_seen_;
      CandRow row;
      row.c = cand;
      // Copy the label under the sampler rather than at print time: by then
      // the operator may already have moved on to the next session.
      size_t i = 0;
      for (; i + 1 < sizeof(row.label) && label_[i] != '\0'; ++i)
        row.label[i] = label_[i];
      row.label[i] = '\0';
      if (xQueueSend(cand_q_, &row, 0) != pdTRUE) ++cand_drop_;
    }

    if (!fired) continue;

    BumpEvent out;
    out.t_ms    = millis();
    out.peak_mg = ev.peak_mg;
    out.axis    = ev.axis;
    out.flags   = ev.flags;
    out.seq     = ev.seq;
    if (xQueueSend(queue_, &out, 0) != pdTRUE) ++drops_;
  }
}

bool BumpDetector::poll(BumpEvent* out) {
  if (queue_ == nullptr || out == nullptr) return false;
  return xQueueReceive(queue_, out, 0) == pdTRUE;
}

void BumpDetector::setLabel(const char* label) {
  size_t i = 0;
  if (label != nullptr)
    for (; i + 1 < sizeof(label_) && label[i] != '\0'; ++i)
      label_[i] = label[i];
  label_[i] = '\0';
}

bool BumpDetector::pollCandidate(bump::Candidate* out, char* label_out,
                                 size_t label_cap) {
  if (cand_q_ == nullptr || out == nullptr) return false;
  CandRow row;
  if (xQueueReceive(cand_q_, &row, 0) != pdTRUE) return false;
  *out = row.c;
  if (label_out != nullptr && label_cap > 0) {
    size_t i = 0;
    for (; i + 1 < label_cap && row.label[i] != '\0'; ++i)
      label_out[i] = row.label[i];
    label_out[i] = '\0';
  }
  return true;
}

void BumpDetector::pause() {
  paused_ = true;
}

void BumpDetector::resume() {
  // Re-seed gravity and re-blank detection. Without this the gravity step
  // across the NFC gap fires a false bump on every scan.
  logic_.reset();
  paused_ = false;
}

void BumpDetector::clearStats() {
  logic_.clearStats();
  samples_      = 0;
  empty_        = 0;
  late_         = 0;
  worst_gap_us_ = 0;
  drops_        = 0;
  started_ms_   = millis();
}

AccelReading BumpDetector::lastReading() const {
  // Copied without a lock. A torn read can only mix two samples 2.5 ms apart,
  // which is invisible in a console readout and not worth a mutex on the
  // sampling path.
  return last_;
}

uint32_t BumpDetector::sampleRateHz() const {
  const uint32_t elapsed = millis() - started_ms_;
  if (elapsed < 1000u) return 0;
  return static_cast<uint32_t>((static_cast<uint64_t>(samples_) * 1000u) /
                               elapsed);
}

bool BumpDetector::saveConfig() {
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  const size_t n = p.putBytes(NVS_KEY, &config(), sizeof(bump::Config));
  // Version first-class alongside the blob, not inside it, so an old blob
  // written by an older build is detectable rather than merely wrong.
  const bool v = p.putUInt(NVS_VER, bump::kConfigVersion) == sizeof(uint32_t);
  p.end();
  return n == sizeof(bump::Config) && v;
}

bool BumpDetector::loadConfig() {
  Preferences p;
  if (!p.begin(NVS_NS, true)) {
    config() = bump::kDefaults;
    return false;
  }
  // Two gates, and the size check alone is not enough: every Config field is
  // an int32_t, so reordering them keeps sizeof() identical and a stale blob
  // would load values into the wrong fields with nothing to signal it.
  const uint32_t ver = p.getUInt(NVS_VER, 0);
  const size_t have  = p.getBytesLength(NVS_KEY);
  bool ok            = false;
  if (ver == bump::kConfigVersion && have == sizeof(bump::Config)) {
    bump::Config stored{};
    if (p.getBytes(NVS_KEY, &stored, sizeof(stored)) == sizeof(stored)) {
      config() = stored;
      ok       = true;
    }
  }
  p.end();
  if (!ok) config() = bump::kDefaults;
  return ok;
}

void BumpDetector::resetConfig() {
  config() = bump::kDefaults;
}

}  // namespace badge
