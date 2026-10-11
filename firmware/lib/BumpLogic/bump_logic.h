// bump_logic.h — deliberate-knock detection, pure integer logic.
//
// Portable on purpose: <stdint.h> only, no Arduino headers, so the native test
// env can exercise every discriminator against synthetic waveforms. If this
// file ever fails to link under [env:native], someone has pulled a hardware
// header into pure logic.
//
// WHAT THIS SOLVES. Sensing an impact is easy. Sensing *the user's deliberate
// knock* and nothing else is not, because the badge is a ~30 g board hanging
// from a lanyard on a walking person in a room where other people slam tables.
// Four tests run in order, cheapest first:
//
//   1. AMPLITUDE   — kills walking. A footfall at the chest is 500-1200 mg
//                    against a 1500 mg threshold. Isolation cannot do this
//                    job (see below), amplitude can.
//   2. ISOLATION   — kills knocking into pre-existing motion. Leaky, not a
//                    hard reset: a blip 150 ms before a real knock should cost
//                    a little credit, not the whole gesture.
//   3. RINGDOWN    — kills a neighbour slamming the desk. This is the one that
//                    earns its keep. A knock dumps energy into damped tissue
//                    or a massive desk and dies in 10-40 ms. A slam excites a
//                    low-damping desk plate that RINGS for 50-200 ms while the
//                    badge rides along. Both "decay", so decay is not the
//                    separator; how many times the signal comes back UP is.
//   4. REFRACTORY  — enforces exactly-one-event-per-knock.
//
// Two things that look like details and are not:
//   * A separate, SHORTER lockout after a *rejected* candidate. Without it a
//     desk ringdown that poks above threshold five times gets five independent
//     chances to pass by luck.
//   * Quiet credit is zeroed when a lockout expires. Otherwise stale credit
//     earned before the rejection lets the next trigger skip isolation.
//
// Units are milligravities throughout, matching badge::AccelReading. All
// durations are in FEED ITERATIONS, not milliseconds, so the logic is
// independent of the rate the caller feeds it.
//
// ONE ITERATION IS ONE NEW SAMPLE, not one poll. The firmware polls every 2 ms
// against a 400 Hz output rate and only feeds samples the sensor actually
// marked as new, so roughly one poll in five yields nothing and
// 1 iteration = 2.5 ms. The constants below are derived from that, and the
// console converts between ms and iterations with the same number. Feed this
// at a different rate and you must re-derive them, or every timing shifts.

#pragma once

#include <stdint.h>

namespace bump {

// Gravity tracker shift. tau = 2^kEmaShift iterations; at 2 ms that is 256 ms,
// a corner near 0.6 Hz — deliberately BELOW walking cadence (1.8-2.2 Hz) so
// that gait still shows up in the signal for the vetoes to see.
constexpr int kEmaShift = 7;

// Fixed-point fraction for the tracker. MUST be >= kEmaShift. In Q0 with a
// shift of 7 a 100 mg offset gives (100 >> 7) == 0 and the tracker never
// converges — it stalls at zero forever and every sample looks like a bump.
constexpr int kEmaFrac = 8;
static_assert(kEmaFrac >= kEmaShift,
              "EMA fraction must be >= shift or the tracker stalls");

// Iterations of blanking after a reset while the gravity estimate settles.
constexpr int32_t kWarmIters = 320;

// Per-axis rail at +/-8 g in high-resolution mode: 2040 counts x 4 mg.
constexpr int32_t kRailMg = 8160;

// Bump this whenever a Config field is added, removed or reordered. Every
// field is an int32_t, so inserting one keeps sizeof(Config) unchanged for any
// swap and a stale NVS blob would load SILENTLY MIS-ASSIGNED values — your
// refractory arriving in dhot_max, with no error anywhere. A size check alone
// cannot catch that; a version tag can.
constexpr uint32_t kConfigVersion = 1;

struct Config {
  int32_t thr_mg;    // peak threshold (1500 mg = 1.5 g)
  int32_t qgate_mg;  // "quiet" means the L1 AC magnitude is under this
  int32_t qneed;     // quiet credit required to arm, iterations
  int32_t qpen;      // credit lost per noisy sample (leaky, not a reset)
  int32_t dshift;    // ringdown gate = thr_mg >> dshift
  int32_t dwin;      // ringdown observation window, iterations
  int32_t dhot_max;  // max samples above the ringdown gate inside the window
  int32_t dq_need;   // consecutive quiet samples needed to confirm
  int32_t refr;      // refractory after an ACCEPTED event, iterations
  int32_t rlock;     // lockout after a REJECTED candidate, iterations
  int32_t agit_max;  // agitation veto level
  int32_t agit_en;   // 1 = agitation veto active
};

// Milliseconds per iteration at the firmware's 400 Hz sample rate. Durations
// below are written as ms / kMsPerIter so the intent stays readable and a rate
// change is one edit rather than twelve.
constexpr int32_t kMsPerIter = 2;  // 2.5 ms rounded down: timings err SHORT,
                                   // which is the safe direction for a
                                   // refractory (never swallow a real knock)

// qneed is deliberately SHORTER than the 350-450 ms quiet gap between
// footfalls. A window long enough to beat gait would be satisfied on every
// stride anyway, and it would wreck knocking while walking — a real gesture.
// Amplitude is what rejects gait.
constexpr Config kDefaults = {
    /*thr_mg  */ 1500,
    /*qgate_mg*/ 240,
    /*qneed   */ 80,  // 200 ms
    /*qpen    */ 8,
    /*dshift  */ 2,    // gate = thr/4 = 375 mg
    /*dwin    */ 24,   // 60 ms: a hand dies in 10-40 ms, a desk rings for
                       //        50-200 ms, so the window sits in the gap
    /*dhot_max*/ 8,    // of 24. The main anti-slam knob; sweep 8 -> 5 -> 3
    /*dq_need */ 4,    // 10 ms of quiet to call it over
    /*refr    */ 100,  // 250 ms. Longer than any ringdown, and longer than a
                       //         human double-knock floor (~100-150 ms)
    /*rlock   */ 40,   // 100 ms
    /*agit_max*/ 600,
    /*agit_en */ 1,
};

enum Flag : uint8_t {
  kFlagClipped  = 0x01,  // hit the rail: peak is a floor, not a measurement
  kFlagWeak     = 0x02,  // under 1.5x threshold; a cautious consumer may skip
  kFlagUnvetted = 0x04,  // agitation veto was off, so lower confidence
};

enum Axis : uint8_t { kAxisX = 0, kAxisY = 1, kAxisZ = 2 };

struct Event {
  int32_t peak_mg;  // peak L1 AC magnitude over the ringdown window
  uint8_t axis;     // dominant axis at the peak sample. A HINT, not a promise
  uint8_t flags;    // Flag bitmask
  uint32_t seq;     // counts DETECTIONS; a gap downstream means a dropped event
};

// ── on-device learned decision ─────────────────────────────────────────────
//
// The hand-tuned rule for the final accept is `hot <= dhot_max && qcons >=
// dq_need`: two integer comparisons chosen by sweeping them on a bench. This
// is the learned alternative — a logistic regression over the five features
// the detector has already computed by the time a candidate's ringdown window
// closes, evaluated in fixed-point integers on the badge.
//
// It is deliberately NOT a replacement by default. `use_model` switches
// between the two so both can be measured on the same hardware against the
// same acceptance protocol, which is the only honest way to claim the learned
// rule is better (or to find out it isn't).
//
// Weights are trained by tools/train_knock.py from candidates captured off
// the badge, and exported as a header. Scale: score = (w0 + Σ wᵢ·fᵢ) >> shift,
// accept when score > 0. Every feature is pre-scaled to stay well inside
// int32 (worst case ≈ 10⁷ before the shift).
struct Model {
  int32_t w0, w_peak, w_hot, w_qcons, w_qrun, w_agit;
  int32_t shift;
  int32_t trained;  // 0 = placeholder, never trained; refuse to use it
};

// Placeholder. train_knock.py overwrites this with real weights; trained=0
// makes the detector fall back to the hand-tuned rule rather than silently
// classifying with zeros.
constexpr Model kUntrainedModel = {0, 0, 0, 0, 0, 0, 0, 0};

// One armed candidate and what the detector decided about it. Emitted for
// ACCEPTED AND REJECTED candidates alike — a classifier trained only on the
// accepted ones has no negative class and cannot learn anything.
struct Candidate {
  int32_t peak_mg;   // peak L1 magnitude over the window
  int32_t hot;       // samples above the ringdown gate
  int32_t qcons;     // trailing consecutive quiet samples
  int32_t qrun_arm;  // isolation credit at the moment it armed
  int32_t agit_arm;  // agitation level at the moment it armed
  uint8_t axis;
  uint8_t clipped;
  uint8_t accepted;  // the verdict the ACTIVE rule reached
  uint8_t by_model;  // 1 if the learned rule decided, 0 if hand-tuned
  int32_t score;     // model score when by_model, else 0
};

struct Stats {
  uint32_t samples;
  uint32_t events;
  uint32_t rej_quiet;  // vetoed by isolation
  uint32_t rej_agit;   // vetoed by agitation
  uint32_t rej_decay;  // vetoed by ringdown
  int32_t mag_now;
  int32_t mag_max;
  int32_t qrun;  // current quiet credit
  int32_t agit;  // current agitation level
};

class Detector {
 public:
  Detector();

  // Drops the gravity estimate and blanks detection for kWarmIters samples.
  // Call this after any gap in sampling — notably around an NFC transaction.
  // Without it the gravity step across the gap fires a false bump every time.
  void reset();

  // Feed one sample. Returns true exactly once per accepted knock, writing the
  // event to `out` (which may be null if the caller only wants the side
  // effects). O(1): no allocation, no division, no sqrt, six scalars of state.
  //
  // `cand`, when non-null, is filled on every sample where a candidate's
  // ringdown window closes — accepted or rejected — and `cand_valid` is set
  // to say whether it was written this call. That is the training-capture
  // path; pass nullptr in normal operation and it costs nothing.
  bool feed(int16_t x_mg, int16_t y_mg, int16_t z_mg, Event* out,
            Candidate* cand = nullptr, bool* cand_valid = nullptr);

  // The learned rule. Off until a trained model is installed.
  Model& model() { return model_; }
  const Model& model() const { return model_; }
  void useModel(bool on) { use_model_ = on && model_.trained != 0; }
  bool usingModel() const { return use_model_; }

  Config& config() { return cfg_; }
  const Config& config() const { return cfg_; }

  const Stats& stats() const { return st_; }
  void clearStats();

  // Last computed L1 AC magnitude — handy for a console readout without
  // touching the I2C bus.
  int32_t magnitude() const { return st_.mag_now; }

 private:
  Config cfg_;
  Stats st_;

  int32_t ema_[3];
  int32_t qrun_;
  int32_t agit_;
  int32_t warm_;
  int32_t lock_;
  int32_t dn_;
  int32_t peak_;
  int32_t hot_;
  int32_t qcons_;
  int32_t qrun_arm_;  // isolation credit captured when the candidate armed
  int32_t agit_arm_;
  uint8_t pk_axis_;
  bool in_decay_;
  bool pk_clipped_;

  Model model_    = kUntrainedModel;
  bool use_model_ = false;
};

}  // namespace bump
