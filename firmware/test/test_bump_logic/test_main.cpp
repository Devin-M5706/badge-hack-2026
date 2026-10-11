// Host-side tests for knock detection.
//   pio test -e native
//
// These run with no badge attached, and they exist because every failure mode
// of this detector is silent on hardware. A stalled gravity tracker, a veto
// that never fires, a refractory that lets one knock through twice — the board
// looks alive in all of them, the LED just blinks at the wrong times, and you
// cannot tell which of four discriminators is wrong while standing up holding
// a lanyard. Here each one is isolated and the waveform is exact.
//
// The synthetic waveforms are deliberately crude: a knock is a tall spike that
// dies immediately, a desk slam is a decaying ring, a footfall is a medium
// bump repeating at gait cadence. Real signals are messier, which is what the
// on-hardware acceptance protocol is for. These tests prove the LOGIC does
// what it claims, not that the thresholds are right for a real badge.

#include <unity.h>

#include "bump_logic.h"

using namespace bump;

namespace {

// Gravity on Z, nothing happening. The detector must see a quiet badge.
constexpr int16_t kRestZ = 1000;

// Feed n still samples. Returns how many events fired (expected: zero).
int feedStill(Detector& d, int n) {
  int fired = 0;
  Event ev;
  for (int i = 0; i < n; ++i) {
    if (d.feed(0, 0, kRestZ, &ev)) ++fired;
  }
  return fired;
}

// A knock: one tall sample, then straight back to rest. Real impacts last
// 10-40 ms in hand; `width` lets a test widen it.
int feedKnock(Detector& d, int16_t peak_mg, int width = 2) {
  int fired = 0;
  Event ev;
  for (int i = 0; i < width; ++i) {
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + peak_mg), &ev)) ++fired;
  }
  // Let the ringdown window close and the refractory expire.
  fired += feedStill(d, kDefaults.dwin + kDefaults.refr + 8);
  return fired;
}

// Settle the gravity tracker past its warm-up blanking.
void warmUp(Detector& d) { feedStill(d, kWarmIters + 8); }

}  // namespace

// ── The gravity tracker ────────────────────────────────────────────────────

void test_still_badge_produces_no_events() {
  Detector d;
  warmUp(d);
  TEST_ASSERT_EQUAL_INT(0, feedStill(d, 2000));
}

void test_gravity_is_removed_so_a_tilted_badge_is_still_quiet() {
  // 1 g on X instead of Z. A detector that thresholds raw magnitude rather
  // than the AC component would fire forever here.
  Detector d;
  Event ev;
  for (int i = 0; i < kWarmIters + 2000; ++i) d.feed(1000, 0, 0, &ev);
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().events);
  TEST_ASSERT_LESS_THAN_INT32(kDefaults.qgate_mg, d.magnitude());
}

void test_tracker_converges_rather_than_stalling() {
  // The kEmaFrac >= kEmaShift rule. In Q0 with a shift of 7, a 100 mg offset
  // gives (100 >> 7) == 0 and the tracker never moves, so every sample looks
  // like a bump forever. After warm-up on a constant input the AC magnitude
  // must be essentially zero.
  Detector d;
  warmUp(d);
  TEST_ASSERT_LESS_THAN_INT32(8, d.magnitude());
}

// ── Amplitude: the test that actually rejects walking ──────────────────────

void test_a_hard_knock_fires_exactly_once() {
  Detector d;
  warmUp(d);
  TEST_ASSERT_EQUAL_INT(1, feedKnock(d, 3000));
}

void test_a_sub_threshold_tap_is_ignored() {
  Detector d;
  warmUp(d);
  TEST_ASSERT_EQUAL_INT(0, feedKnock(d, kDefaults.thr_mg - 200));
}

void test_walking_cadence_does_not_fire() {
  // Footfalls at the chest: ~800 mg, 100 ms wide, every ~500 ms. Below the
  // 1500 mg threshold, so amplitude alone must reject it. This is the case
  // isolation structurally CANNOT handle: the 400 ms quiet gap between
  // footfalls exceeds any usable quiet-credit window.
  Detector d;
  warmUp(d);
  int fired = 0;
  Event ev;
  for (int step = 0; step < 20; ++step) {
    for (int i = 0; i < 40; ++i) {  // ~100 ms of footfall
      if (d.feed(0, 0, static_cast<int16_t>(kRestZ + 800), &ev)) ++fired;
    }
    for (int i = 0; i < 160; ++i) {  // ~400 ms of quiet
      if (d.feed(0, 0, kRestZ, &ev)) ++fired;
    }
  }
  TEST_ASSERT_EQUAL_INT(0, fired);
}

// ── Ringdown: the test that rejects a neighbour's desk slam ────────────────

void test_a_ringing_desk_slam_is_rejected() {
  // A slam couples through a low-damping desk plate: tall first lobe, then it
  // keeps coming back up for 50-200 ms. Both a knock and a slam "decay", so
  // decay is not the separator — how many times the signal re-crosses the
  // gate is. This waveform stays hot for the whole window.
  Detector d;
  warmUp(d);
  Event ev;
  int fired = 0;
  int16_t amp = 3000;
  for (int i = 0; i < kDefaults.dwin + 4; ++i) {
    // Oscillate above and below the ringdown gate without settling.
    const int16_t v = (i % 2 == 0) ? amp : static_cast<int16_t>(amp / 2);
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + v), &ev)) ++fired;
    if (amp > 900) amp = static_cast<int16_t>(amp - 60);
  }
  fired += feedStill(d, kDefaults.refr + 8);
  TEST_ASSERT_EQUAL_INT(0, fired);
  TEST_ASSERT_GREATER_THAN_UINT32(0, d.stats().rej_decay);
}

void test_a_sustained_push_is_rejected() {
  // Squeezing or pressing the badge: crosses the threshold and stays there.
  // It never goes quiet, so the confirmation requirement rejects it.
  Detector d;
  warmUp(d);
  Event ev;
  int fired = 0;
  for (int i = 0; i < 400; ++i) {
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + 2500), &ev)) ++fired;
  }
  TEST_ASSERT_EQUAL_INT(0, fired);
}

// ── Isolation and its leakiness ────────────────────────────────────────────

void test_knocking_into_ongoing_motion_is_vetoed() {
  Detector d;
  warmUp(d);
  Event ev;
  // Continuous agitation just under threshold, then a knock on top of it.
  for (int i = 0; i < 300; ++i) {
    const int16_t v = (i % 2 == 0) ? 700 : -700;
    d.feed(0, 0, static_cast<int16_t>(kRestZ + v), &ev);
  }
  int fired = 0;
  for (int i = 0; i < 2; ++i) {
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &ev)) ++fired;
  }
  fired += feedStill(d, kDefaults.dwin + kDefaults.refr + 8);
  TEST_ASSERT_EQUAL_INT(0, fired);
}

void test_one_blip_long_before_a_knock_does_not_block_it() {
  // Leaky credit, not a hard reset: a single noisy sample well before the
  // knock should cost a little credit, not the whole gesture.
  Detector d;
  warmUp(d);
  Event ev;
  d.feed(0, 0, static_cast<int16_t>(kRestZ + 500), &ev);  // one blip
  feedStill(d, kDefaults.qneed + 20);                      // re-earn credit
  TEST_ASSERT_EQUAL_INT(1, feedKnock(d, 3000));
}

// ── Refractory and the post-reject lockout ────────────────────────────────

void test_two_knocks_inside_the_refractory_yield_one_event() {
  Detector d;
  warmUp(d);
  Event ev;
  int fired = 0;
  for (int i = 0; i < 2; ++i) {
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &ev)) ++fired;
  }
  // Second strike only ~40 ms later, inside both the window and the refractory.
  feedStill(d, 16);
  for (int i = 0; i < 2; ++i) {
    if (d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &ev)) ++fired;
  }
  fired += feedStill(d, kDefaults.dwin + kDefaults.refr + 8);
  TEST_ASSERT_EQUAL_INT(1, fired);
}

void test_two_knocks_separated_by_more_than_the_refractory_both_fire() {
  Detector d;
  warmUp(d);
  TEST_ASSERT_EQUAL_INT(1, feedKnock(d, 3000));
  feedStill(d, kDefaults.qneed + 20);
  TEST_ASSERT_EQUAL_INT(1, feedKnock(d, 3000));
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().events);
}

void test_sequence_numbers_are_monotonic() {
  Detector d;
  warmUp(d);
  Event first{}, second{};
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &first);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &first);
  feedStill(d, kDefaults.refr + kDefaults.qneed + 20);
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &second);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &second);
  TEST_ASSERT_EQUAL_UINT32(1, first.seq);
  TEST_ASSERT_EQUAL_UINT32(2, second.seq);
}

// ── Reset semantics: the NFC gap ──────────────────────────────────────────

void test_reset_blanks_detection_so_a_gravity_step_does_not_fire() {
  // Polling stops for an NFC scan and the badge is re-oriented meanwhile. On
  // resume the gravity estimate is stale by a full g. Without reset() that
  // step is indistinguishable from an impact and fires on every single scan.
  Detector d;
  warmUp(d);
  d.reset();
  int fired = 0;
  Event ev;
  for (int i = 0; i < kWarmIters; ++i) {
    if (d.feed(1000, 0, 0, &ev)) ++fired;  // gravity now on X, not Z
  }
  TEST_ASSERT_EQUAL_INT(0, fired);
}

// ── Event payload ─────────────────────────────────────────────────────────

void test_axis_hint_reports_the_struck_axis() {
  Detector d;
  warmUp(d);
  Event ev{};
  for (int i = 0; i < 2; ++i) d.feed(3000, 0, kRestZ, &ev);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &ev);
  TEST_ASSERT_EQUAL_UINT8(kAxisX, ev.axis);
}

void test_a_marginal_knock_is_flagged_weak() {
  Detector d;
  warmUp(d);
  Event ev{};
  const int16_t marginal = static_cast<int16_t>(kDefaults.thr_mg + 100);
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRestZ + marginal), &ev);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &ev);
  TEST_ASSERT_TRUE((ev.flags & kFlagWeak) != 0);
}

void test_disabling_the_agitation_veto_marks_events_unvetted() {
  // A consumer should be able to tell that an event was not screened for
  // repetitive motion, rather than trusting it equally.
  Detector d;
  d.config().agit_en = 0;
  warmUp(d);
  Event ev{};
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &ev);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &ev);
  TEST_ASSERT_TRUE((ev.flags & kFlagUnvetted) != 0);
}

void test_a_railed_knock_is_flagged_clipped() {
  Detector d;
  warmUp(d);
  Event ev{};
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRailMg), &ev);
  for (int i = 0; i < kDefaults.dwin; ++i) d.feed(0, 0, kRestZ, &ev);
  TEST_ASSERT_TRUE((ev.flags & kFlagClipped) != 0);
}

// ── Stats ────────────────────────────────────────────────────────────────

void test_rejection_counters_attribute_the_veto() {
  // When tuning, "it did not fire" is useless; "it was vetoed by isolation"
  // tells you which knob to turn.
  Detector d;
  warmUp(d);
  Event ev;
  for (int i = 0; i < 300; ++i) {
    const int16_t v = (i % 2 == 0) ? 700 : -700;
    d.feed(0, 0, static_cast<int16_t>(kRestZ + v), &ev);
  }
  for (int i = 0; i < 2; ++i) d.feed(0, 0, static_cast<int16_t>(kRestZ + 3000), &ev);
  TEST_ASSERT_GREATER_THAN_UINT32(0, d.stats().rej_quiet + d.stats().rej_agit);
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().events);
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();

  RUN_TEST(test_still_badge_produces_no_events);
  RUN_TEST(test_gravity_is_removed_so_a_tilted_badge_is_still_quiet);
  RUN_TEST(test_tracker_converges_rather_than_stalling);

  RUN_TEST(test_a_hard_knock_fires_exactly_once);
  RUN_TEST(test_a_sub_threshold_tap_is_ignored);
  RUN_TEST(test_walking_cadence_does_not_fire);

  RUN_TEST(test_a_ringing_desk_slam_is_rejected);
  RUN_TEST(test_a_sustained_push_is_rejected);

  RUN_TEST(test_knocking_into_ongoing_motion_is_vetoed);
  RUN_TEST(test_one_blip_long_before_a_knock_does_not_block_it);

  RUN_TEST(test_two_knocks_inside_the_refractory_yield_one_event);
  RUN_TEST(test_two_knocks_separated_by_more_than_the_refractory_both_fire);
  RUN_TEST(test_sequence_numbers_are_monotonic);

  RUN_TEST(test_reset_blanks_detection_so_a_gravity_step_does_not_fire);

  RUN_TEST(test_axis_hint_reports_the_struck_axis);
  RUN_TEST(test_a_marginal_knock_is_flagged_weak);
  RUN_TEST(test_disabling_the_agitation_veto_marks_events_unvetted);
  RUN_TEST(test_a_railed_knock_is_flagged_clipped);

  RUN_TEST(test_rejection_counters_attribute_the_veto);

  return UNITY_END();
}
