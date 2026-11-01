// Native tests for the TX clamp and band checks. Run with:
//   pio test -e tasnode_test

#include <cstring>

#include <unity.h>

#include "tasnode/radio_limits.h"

using namespace tasnode;

void setUp() {}
void tearDown() {}

static const uint32_t CENTRE_HZ = 920000000;

static void assert_near(double expected, double actual) {
  TEST_ASSERT_TRUE(std::fabs(expected - actual) < 0.05);
}

void test_psd_limit_matches_brief() {
  assert_near(30.2, psd_limit_dbm(125000));
  assert_near(27.2, psd_limit_dbm(62500));
  assert_near(24.2, psd_limit_dbm(31250));
}

void test_eirp_capped_at_one_watt() {
  TEST_ASSERT_EQUAL_FLOAT(30.0, max_eirp_dbm(125000));
  TEST_ASSERT_EQUAL_FLOAT(30.0, max_eirp_dbm(500000));
  assert_near(27.2, max_eirp_dbm(62500));
}

// 3 bandwidths x 3 antenna gains. Worked by hand from section 7.2:
//                 3 dBi            6 dBi            9 dBi
//   125 kHz    min(22, 27) = 22  min(22, 24) = 22  21
//    62.5 kHz  min(22, 24.2)=22  21.2 -> 21        18.2 -> 18
//    31.25 kHz 21.2 -> 21        18.2 -> 18        15.2 -> 15
void test_matrix_bandwidth_by_gain() {
  const uint32_t bws[3] = {125000, 62500, 31250};
  const int16_t gains[3] = {30, 60, 90};
  const int expected[3][3] = {{22, 22, 21}, {22, 21, 18}, {21, 18, 15}};
  for (int b = 0; b < 3; b++) {
    for (int g = 0; g < 3; g++) {
      TEST_ASSERT_EQUAL_INT(expected[b][g], max_tx_dbm(bws[b], gains[g], 0));
    }
  }
}

void test_radio_cap_governs_with_low_gain() {
  TEST_ASSERT_EQUAL_INT(22, max_tx_dbm(125000, 0, 0));
  TEST_ASSERT_EQUAL_INT(22, max_tx_dbm(500000, -100, 0));
}

void test_psd_governs_at_narrow_bandwidth() {
  // 7.8 kHz: 25 mW x 2.6 = 65 mW, 18.1 dBm. Less 3 dBi = 15.1.
  TEST_ASSERT_EQUAL_INT(15, max_tx_dbm(7800, 30, 0));
}

void test_feeder_loss_adds_back() {
  TEST_ASSERT_EQUAL_INT(21, max_tx_dbm(125000, 90, 0));
  TEST_ASSERT_EQUAL_INT(22, max_tx_dbm(125000, 90, 10));
  TEST_ASSERT_EQUAL_INT(15, max_tx_dbm(31250, 90, 0));
  TEST_ASSERT_EQUAL_INT(17, max_tx_dbm(31250, 90, 20));
}

void test_rounds_down_never_up() {
  // 30 - 8.1 = 21.9 must not become 22.
  TEST_ASSERT_EQUAL_INT(21, max_tx_dbm(125000, 81, 0));
  // Gain minus loss is exactly 8 dB here, so this is exactly 22.
  TEST_ASSERT_EQUAL_INT(22, max_tx_dbm(125000, 83, 3));
}

void test_band_edges() {
  TEST_ASSERT_FALSE(freq_allowed(914999999));
  TEST_ASSERT_FALSE(freq_allowed(915000000));
  TEST_ASSERT_TRUE(freq_allowed(915000001));
  TEST_ASSERT_TRUE(freq_allowed(915001000));
  TEST_ASSERT_TRUE(freq_allowed(CENTRE_HZ));
  TEST_ASSERT_TRUE(freq_allowed(928000000));
  TEST_ASSERT_FALSE(freq_allowed(928000001));
  TEST_ASSERT_FALSE(freq_allowed(928001000));
}

// 125 kHz is 62.5 kHz either side of centre. The lower edge is exclusive.
void test_whole_signal_edges_125khz() {
  TEST_ASSERT_FALSE(signal_in_band(915062500, 125000));
  TEST_ASSERT_TRUE(signal_in_band(915062501, 125000));
  TEST_ASSERT_TRUE(signal_in_band(927937500, 125000));
  TEST_ASSERT_FALSE(signal_in_band(927937501, 125000));
}

void test_whole_signal_edges_scale_with_bandwidth() {
  TEST_ASSERT_FALSE(signal_in_band(915250000, 500000));
  TEST_ASSERT_TRUE(signal_in_band(915250001, 500000));
  TEST_ASSERT_TRUE(signal_in_band(915062501, 125000));
  TEST_ASSERT_FALSE(signal_in_band(915062501, 500000));
  // 62.5 kHz is 31.25 kHz either side, so the half-kHz is kept exactly.
  TEST_ASSERT_FALSE(signal_in_band(915031250, 62500));
  TEST_ASSERT_TRUE(signal_in_band(915031251, 62500));
}

void test_check_refuses_signal_over_the_edge() {
  // Both centres are in band, but the signal isn't.
  TEST_ASSERT_TRUE(freq_allowed(915010000));
  TEST_ASSERT_TRUE(check_radio(915010000, 125000, 2, 30, 0).verdict == Verdict::FreqOutOfBand);
  TEST_ASSERT_TRUE(freq_allowed(928000000));
  TEST_ASSERT_TRUE(check_radio(928000000, 125000, 2, 30, 0).verdict == Verdict::FreqOutOfBand);
}

void test_frequency_step_passes_until_bandwidth_is_known() {
  // Hosts send frequency first, so a centre near an edge can't be judged yet.
  TEST_ASSERT_TRUE(check_radio(915010000, 0, UNSET_TXP, 30, 0).verdict == Verdict::Ok);
}

void test_scotmesh_presets_are_refused() {
  TEST_ASSERT_FALSE(freq_allowed(869462500));
  TEST_ASSERT_FALSE(freq_allowed(433775000));
}

void test_check_accepts_at_limit() {
  const Check c = check_radio(CENTRE_HZ, 31250, 21, 30, 0);
  TEST_ASSERT_TRUE(c.verdict == Verdict::Ok);
  TEST_ASSERT_EQUAL_INT(21, c.limit_dbm);
}

void test_check_rejects_one_over_limit() {
  const Check c = check_radio(CENTRE_HZ, 31250, 22, 30, 0);
  TEST_ASSERT_TRUE(c.verdict == Verdict::TxOverLimit);
  TEST_ASSERT_EQUAL_INT(21, c.limit_dbm);
}

void test_check_rejects_out_of_band_first() {
  TEST_ASSERT_TRUE(check_radio(915000000, 125000, 2, 30, 0).verdict == Verdict::FreqOutOfBand);
}

void test_check_skips_unset_values() {
  TEST_ASSERT_TRUE(check_radio(UNSET_HZ, 125000, UNSET_TXP, 30, 0).verdict == Verdict::Ok);
  TEST_ASSERT_TRUE(check_radio(CENTRE_HZ, 0, 22, 30, 0).verdict == Verdict::Ok);
  TEST_ASSERT_TRUE(check_radio(915000000, 0, 22, 30, 0).verdict == Verdict::FreqOutOfBand);
}

void test_check_no_legal_power() {
  // 30 dBi at 7.8 kHz leaves 18.1 - 30 = -11.9, below the radio's minimum.
  const Check c = check_radio(CENTRE_HZ, 7800, -9, 300, 0);
  TEST_ASSERT_TRUE(c.verdict == Verdict::NoLegalPower);
}

void test_error_text_names_the_limit() {
  char msg[64];
  const Check c = check_radio(CENTRE_HZ, 31250, 22, 30, 0);
  format_error(c, 22, msg, sizeof msg);
  TEST_ASSERT_NOT_NULL(strstr(msg, "limit 21 dBm"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "22 dBm"));
}

void test_error_text_names_the_band() {
  char msg[64];
  format_error(check_radio(915000000, 125000, 2, 30, 0), 2, msg, sizeof msg);
  TEST_ASSERT_NOT_NULL(strstr(msg, "915.000"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "928.000"));
}

void test_tasmania_preset_is_legal_at_default_antenna() {
  const int txp = max_tx_dbm(PRESET_BW_HZ, DEFAULT_GAIN_TENTHS, DEFAULT_LOSS_TENTHS);
  TEST_ASSERT_EQUAL_INT(22, txp);
  TEST_ASSERT_TRUE(check_radio(PRESET_FREQ_HZ, PRESET_BW_HZ, txp, DEFAULT_GAIN_TENTHS,
                               DEFAULT_LOSS_TENTHS).verdict == Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT32(125000, PRESET_BW_HZ);
  TEST_ASSERT_EQUAL_INT(9, PRESET_SF);
  TEST_ASSERT_EQUAL_INT(5, PRESET_CR);
}

void test_antenna_ranges() {
  TEST_ASSERT_TRUE(gain_in_range(30));
  TEST_ASSERT_FALSE(gain_in_range(301));
  TEST_ASSERT_FALSE(gain_in_range(-101));
  TEST_ASSERT_TRUE(loss_in_range(0));
  TEST_ASSERT_FALSE(loss_in_range(-1));
  TEST_ASSERT_FALSE(loss_in_range(201));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_psd_limit_matches_brief);
  RUN_TEST(test_eirp_capped_at_one_watt);
  RUN_TEST(test_matrix_bandwidth_by_gain);
  RUN_TEST(test_radio_cap_governs_with_low_gain);
  RUN_TEST(test_psd_governs_at_narrow_bandwidth);
  RUN_TEST(test_feeder_loss_adds_back);
  RUN_TEST(test_rounds_down_never_up);
  RUN_TEST(test_band_edges);
  RUN_TEST(test_whole_signal_edges_125khz);
  RUN_TEST(test_whole_signal_edges_scale_with_bandwidth);
  RUN_TEST(test_check_refuses_signal_over_the_edge);
  RUN_TEST(test_frequency_step_passes_until_bandwidth_is_known);
  RUN_TEST(test_scotmesh_presets_are_refused);
  RUN_TEST(test_check_accepts_at_limit);
  RUN_TEST(test_check_rejects_one_over_limit);
  RUN_TEST(test_check_rejects_out_of_band_first);
  RUN_TEST(test_check_skips_unset_values);
  RUN_TEST(test_check_no_legal_power);
  RUN_TEST(test_error_text_names_the_limit);
  RUN_TEST(test_error_text_names_the_band);
  RUN_TEST(test_tasmania_preset_is_legal_at_default_antenna);
  RUN_TEST(test_antenna_ranges);
  return UNITY_END();
}
