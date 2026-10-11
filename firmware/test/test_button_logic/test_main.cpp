// Host-side tests for button decoding and debouncing.
//   pio test -e native
//
// These run on your laptop with no badge attached. They exist because the two
// bugs the HAL guide explicitly warns about — inverted active-low decoding and
// Aux1 not booting released — are both silent failures on hardware: the board
// looks alive, the buttons just do the wrong thing, and you lose 40 minutes.

#include <unity.h>

#include "button_logic.h"

using namespace badge;

// ── Decoding (HAL guide §3) ────────────────────────────────────────────────

void test_nothing_pressed_decodes_to_zero() {
  // All inputs high = all released, because buttons are active-low.
  TEST_ASSERT_EQUAL_UINT16(0, decodeShiftByte(0xFF));
}

void test_all_pressed_decodes_to_all_eight_bits() {
  TEST_ASSERT_EQUAL_UINT16(0x00FF, decodeShiftByte(0x00));
}

void test_shift_order_maps_first_bit_to_button_a() {
  // "A shifts out first" — so shift position 0 must be A, not Aux1.
  // Getting this backwards is the classic 74HC165 bug and it is survivable
  // enough on hardware (buttons work, just all wrong) to waste real time.
  const uint16_t mask = decodeShiftByte(0xFE);  // bit 0 low => A pressed
  TEST_ASSERT_TRUE(mask & (1u << BTN_A));
  TEST_ASSERT_FALSE(mask & (1u << BTN_AUX1));
}

void test_shift_order_maps_last_bit_to_aux1() {
  const uint16_t mask = decodeShiftByte(0x7F);  // bit 7 low => Aux1 pressed
  TEST_ASSERT_TRUE(mask & (1u << BTN_AUX1));
  TEST_ASSERT_FALSE(mask & (1u << BTN_A));
}

void test_dpad_order_matches_guide() {
  // Documented order: A, B, Home, Down, Left, Right, Up, Aux1.
  // Note Down comes before Up — transcribing this as the intuitive
  // Up/Down/Left/Right would invert the d-pad vertically.
  TEST_ASSERT_EQUAL_UINT8(2, BTN_HOME);
  TEST_ASSERT_EQUAL_UINT8(3, BTN_DOWN);
  TEST_ASSERT_EQUAL_UINT8(4, BTN_LEFT);
  TEST_ASSERT_EQUAL_UINT8(5, BTN_RIGHT);
  TEST_ASSERT_EQUAL_UINT8(6, BTN_UP);
}

void test_sample_array_and_packed_byte_agree() {
  uint8_t samples[kShiftCount] = {1, 1, 0, 1, 1, 1, 1, 1};  // Home pressed
  TEST_ASSERT_EQUAL_UINT16(decodeShiftByte(0xFB), decodeShiftSamples(samples));
}

// ── Debouncing ─────────────────────────────────────────────────────────────

void test_single_noisy_sample_is_rejected() {
  Debouncer d(2);
  d.update(1u << BTN_A);  // one sample only — contact bounce, not a press
  TEST_ASSERT_FALSE(d.isDown(BTN_A));
}

void test_two_consecutive_samples_accept_the_press() {
  Debouncer d(2);
  d.update(1u << BTN_A);
  d.update(1u << BTN_A);
  TEST_ASSERT_TRUE(d.isDown(BTN_A));
  TEST_ASSERT_TRUE(d.wasPressed(BTN_A));
}

void test_press_edge_fires_once_not_every_update() {
  Debouncer d(2);
  d.update(1u << BTN_A);
  d.update(1u << BTN_A);
  TEST_ASSERT_TRUE(d.wasPressed(BTN_A));
  d.update(1u << BTN_A);  // still held
  TEST_ASSERT_FALSE(d.wasPressed(BTN_A));  // but no new edge
  TEST_ASSERT_TRUE(d.isDown(BTN_A));
}

void test_release_edge_fires_after_debounce() {
  Debouncer d(2);
  d.update(1u << BTN_A);
  d.update(1u << BTN_A);
  d.update(0);
  TEST_ASSERT_TRUE(d.isDown(BTN_A));  // one sample is not enough to release
  d.update(0);
  TEST_ASSERT_FALSE(d.isDown(BTN_A));
  TEST_ASSERT_TRUE(d.wasReleased(BTN_A));
}

void test_bounce_back_to_held_cancels_pending_release() {
  Debouncer d(3);
  d.prime(1u << BTN_A);  // established as held
  d.update(0);           // starts to look released
  d.update(1u << BTN_A); // bounced back
  d.update(0);
  d.update(0);           // only 2 consecutive now, needs 3
  TEST_ASSERT_TRUE(d.isDown(BTN_A));
}

void test_buttons_debounce_independently() {
  Debouncer d(2);
  d.update((1u << BTN_A) | (1u << BTN_UP));
  d.update(1u << BTN_A);  // UP vanished after one sample, A persisted
  TEST_ASSERT_TRUE(d.isDown(BTN_A));
  TEST_ASSERT_FALSE(d.isDown(BTN_UP));
}

// ── The Aux1 boot gotcha (HAL guide §3) ────────────────────────────────────

void test_priming_adopts_state_without_firing_edges() {
  // Aux1 is a maintained side switch — it can boot already held. Priming must
  // record it as down while emitting NO press edge, otherwise an app that
  // switches mode on "Aux1 pressed" silently boots into the wrong mode.
  Debouncer d(2);
  d.prime(1u << BTN_AUX1);
  TEST_ASSERT_TRUE(d.isDown(BTN_AUX1));
  TEST_ASSERT_FALSE(d.wasPressed(BTN_AUX1));
  TEST_ASSERT_EQUAL_UINT16(0, d.pressedEdges());
}

void test_without_priming_a_held_aux1_looks_like_a_fresh_press() {
  // Documents the failure this API prevents. If this ever stops holding, the
  // debouncer changed behaviour and prime() may no longer be needed.
  Debouncer d(2);
  d.update(1u << BTN_AUX1);
  d.update(1u << BTN_AUX1);
  TEST_ASSERT_TRUE(d.wasPressed(BTN_AUX1));
}

void test_releasing_a_primed_switch_still_reports_an_edge() {
  Debouncer d(2);
  d.prime(1u << BTN_AUX1);
  d.update(0);
  d.update(0);
  TEST_ASSERT_TRUE(d.wasReleased(BTN_AUX1));
  TEST_ASSERT_FALSE(d.isDown(BTN_AUX1));
}

// ── Names (used by the serial console) ─────────────────────────────────────

void test_name_round_trips_for_every_button() {
  for (uint8_t i = 0; i < BTN_COUNT; ++i) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(i), buttonFromName(buttonName(i)));
  }
}

void test_name_lookup_is_case_insensitive() {
  TEST_ASSERT_EQUAL_INT(BTN_AUX1, buttonFromName("AUX1"));
  TEST_ASSERT_EQUAL_INT(BTN_START, buttonFromName("Start"));
}

void test_unknown_name_is_rejected() {
  TEST_ASSERT_EQUAL_INT(-1, buttonFromName("select"));
  TEST_ASSERT_EQUAL_INT(-1, buttonFromName(""));
}

// ── Runner ─────────────────────────────────────────────────────────────────

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_nothing_pressed_decodes_to_zero);
  RUN_TEST(test_all_pressed_decodes_to_all_eight_bits);
  RUN_TEST(test_shift_order_maps_first_bit_to_button_a);
  RUN_TEST(test_shift_order_maps_last_bit_to_aux1);
  RUN_TEST(test_dpad_order_matches_guide);
  RUN_TEST(test_sample_array_and_packed_byte_agree);

  RUN_TEST(test_single_noisy_sample_is_rejected);
  RUN_TEST(test_two_consecutive_samples_accept_the_press);
  RUN_TEST(test_press_edge_fires_once_not_every_update);
  RUN_TEST(test_release_edge_fires_after_debounce);
  RUN_TEST(test_bounce_back_to_held_cancels_pending_release);
  RUN_TEST(test_buttons_debounce_independently);

  RUN_TEST(test_priming_adopts_state_without_firing_edges);
  RUN_TEST(test_without_priming_a_held_aux1_looks_like_a_fresh_press);
  RUN_TEST(test_releasing_a_primed_switch_still_reports_an_edge);

  RUN_TEST(test_name_round_trips_for_every_button);
  RUN_TEST(test_name_lookup_is_case_insensitive);
  RUN_TEST(test_unknown_name_is_rejected);

  return UNITY_END();
}
