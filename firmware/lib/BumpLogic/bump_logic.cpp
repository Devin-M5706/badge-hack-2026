#include "bump_logic.h"

namespace bump {
namespace {

inline int32_t iabs32(int32_t v) {
  return v < 0 ? -v : v;
}

}  // namespace

Detector::Detector() : cfg_(kDefaults) {
  clearStats();
  reset();
}

void Detector::clearStats() {
  st_ = Stats{};
}

void Detector::reset() {
  ema_[0] = ema_[1] = ema_[2] = 0;
  qrun_                       = 0;
  agit_                       = 0;
  warm_                       = kWarmIters;
  lock_                       = 0;
  dn_                         = 0;
  peak_                       = 0;
  hot_                        = 0;
  qcons_                      = 0;
  qrun_arm_                   = 0;
  agit_arm_                   = 0;
  pk_axis_                    = kAxisZ;
  in_decay_                   = false;
  pk_clipped_                 = false;
}

// Feature scaling, shared with tools/train_knock.py. Both sides must agree or
// the trained weights mean nothing on the badge.
//   peak  -> mg >> 4   (1500 mg becomes 93)
//   qrun  -> credit >> 4
//   agit  -> level >> 4
//   hot, qcons pass through: both are already small sample counts.
static inline int32_t f_peak(int32_t mg) {
  return mg >> 4;
}
static inline int32_t f_qrun(int32_t cr) {
  return cr >> 4;
}
static inline int32_t f_agit(int32_t ag) {
  return ag >> 4;
}

bool Detector::feed(int16_t x_mg, int16_t y_mg, int16_t z_mg, Event* out,
                    Candidate* cand, bool* cand_valid) {
  if (cand_valid != nullptr) *cand_valid = false;
  const int16_t raw[3] = {x_mg, y_mg, z_mg};
  bool clipped         = false;
  for (int i = 0; i < 3; ++i) {
    if (raw[i] >= kRailMg || raw[i] <= -kRailMg) clipped = true;
  }

  // ── gravity removal, L1 magnitude, axis hint ────────────────────────────
  // L1 rather than the Euclidean norm: no sqrt per sample, and the threshold
  // stays legible in mg. L1 does overstate a diagonal impact by up to ~73%,
  // which is why the acceptance protocol tests edge and face knocks
  // separately instead of assuming one threshold fits every direction.
  int32_t mag  = 0;
  int32_t amax = -1;
  uint8_t ax   = kAxisZ;
  for (int i = 0; i < 3; ++i) {
    const int32_t xq = static_cast<int32_t>(raw[i]) << kEmaFrac;
    if (warm_ == kWarmIters) ema_[i] = xq;  // seed on the first sample
    ema_[i] += (xq - ema_[i]) >> kEmaShift;
    const int32_t ac =
        iabs32(static_cast<int32_t>(raw[i]) - (ema_[i] >> kEmaFrac));
    mag += ac;
    if (ac > amax) {
      amax = ac;
      ax   = static_cast<uint8_t>(i);
    }
  }

  ++st_.samples;
  st_.mag_now = mag;
  if (mag > st_.mag_max) st_.mag_max = mag;

  // ── two leaky integrators ───────────────────────────────────────────────
  // qrun_ is a short horizon: "has it been still just now?"
  // agit_ is a ~2 s horizon: "is this repetitive motion rather than an event?"
  // Isolation's horizon is structurally too short to see cadence, which is
  // exactly what walking and table-drumming look like.
  if (mag < cfg_.qgate_mg) {
    if (qrun_ < 8192) ++qrun_;
    if (agit_ > 0) --agit_;
  } else {
    qrun_ -= cfg_.qpen;
    if (qrun_ < 0) qrun_ = 0;
    agit_ += 8;
    if (agit_ > 8192) agit_ = 8192;
  }
  st_.qrun = qrun_;
  st_.agit = agit_;

  if (warm_ > 0) {
    --warm_;
    return false;
  }

  if (lock_ > 0) {
    // Zeroing credit as the lockout ends is load-bearing: stale credit earned
    // before a rejection would let the very next trigger skip isolation.
    if (--lock_ == 0) qrun_ = 0;
    return false;
  }

  if (!in_decay_) {
    // ── ARMED ────────────────────────────────────────────────────────────
    if (mag < cfg_.thr_mg) return false;

    if (qrun_ < cfg_.qneed) {
      ++st_.rej_quiet;
      lock_ = cfg_.rlock;
      return false;
    }
    if (cfg_.agit_en != 0 && agit_ > cfg_.agit_max) {
      ++st_.rej_agit;
      lock_ = cfg_.rlock;
      return false;
    }

    peak_       = mag;
    pk_axis_    = ax;
    pk_clipped_ = clipped;
    hot_        = 0;
    qcons_      = 0;
    qrun_arm_ =
        qrun_;  // snapshot: by the time the window closes this has moved
    agit_arm_ = agit_;
    dn_       = cfg_.dwin;
    in_decay_ = true;
    return false;
  }

  // ── RINGDOWN window ──────────────────────────────────────────────────────
  if (mag > peak_) {
    peak_    = mag;
    pk_axis_ = ax;
  }
  if (clipped) pk_clipped_ = true;

  if (mag >= (cfg_.thr_mg >> cfg_.dshift)) {
    ++hot_;
    qcons_ = 0;
  } else {
    ++qcons_;
  }

  if (--dn_ > 0) return false;

  in_decay_ = false;
  lock_     = cfg_.refr;  // refractory starts whether we accept or reject

  // ── the final accept: hand-tuned rule, or the learned one ───────────────
  bool accept;
  int32_t score = 0;
  if (use_model_) {
    score = model_.w0 + model_.w_peak * f_peak(peak_) + model_.w_hot * hot_ +
            model_.w_qcons * qcons_ + model_.w_qrun * f_qrun(qrun_arm_) +
            model_.w_agit * f_agit(agit_arm_);
    score >>= model_.shift;
    accept = score > 0;
  } else {
    // It rang, or it never went quiet: a desk, not a knock.
    accept = !(hot_ > cfg_.dhot_max || qcons_ < cfg_.dq_need);
  }

  // Capture every candidate, accepted or not. The rejected ones ARE the
  // negative class; without them there is nothing to train against.
  if (cand != nullptr) {
    cand->peak_mg  = peak_;
    cand->hot      = hot_;
    cand->qcons    = qcons_;
    cand->qrun_arm = qrun_arm_;
    cand->agit_arm = agit_arm_;
    cand->axis     = pk_axis_;
    cand->clipped  = pk_clipped_ ? 1 : 0;
    cand->accepted = accept ? 1 : 0;
    cand->by_model = use_model_ ? 1 : 0;
    cand->score    = score;
    if (cand_valid != nullptr) *cand_valid = true;
  }

  if (!accept) {
    ++st_.rej_decay;
    return false;
  }

  ++st_.events;
  if (out != nullptr) {
    out->peak_mg = peak_;
    out->axis    = pk_axis_;
    out->flags   = static_cast<uint8_t>(
        (pk_clipped_ ? kFlagClipped : 0) |
        (peak_ < cfg_.thr_mg + (cfg_.thr_mg >> 1) ? kFlagWeak : 0) |
        (cfg_.agit_en != 0 ? 0 : kFlagUnvetted));
    out->seq = st_.events;
  }
  return true;
}

}  // namespace bump
